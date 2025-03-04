// Copyright (c) 2016- PPSSPP Project.

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, version 2.0 or later versions.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License 2.0 for more details.

// A copy of the GPL 2.0 should have been included with the program.
// If not, see http://www.gnu.org/licenses/

// Official git repository and contact information can be found at
// https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.

#include "ppsspp_config.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <png.h>

#include "ext/basis_universal/basisu_transcoder.h"
#include "ext/xxhash.h"

#include "Common/Data/Convert/ColorConv.h"
#include "Common/Data/Format/IniFile.h"
#include "Common/Data/Format/ZIMLoad.h"
#include "Common/Data/Format/PNGLoad.h"
#include "Common/Data/Text/I18n.h"
#include "Common/Data/Text/Parsers.h"
#include "Common/File/VFS/DirectoryReader.h"
#include "Common/File/VFS/ZipFileReader.h"
#include "Common/File/FileUtil.h"
#include "Common/File/VFS/VFS.h"
#include "Common/LogReporting.h"
#include "Common/StringUtils.h"
#include "Common/Thread/ParallelLoop.h"
#include "Common/Thread/Waitable.h"
#include "Common/Thread/ThreadManager.h"
#include "Common/TimeUtil.h"
#include "Core/Config.h"
#include "Core/Host.h"
#include "Core/System.h"
#include "Core/ThreadPools.h"
#include "Core/ELF/ParamSFO.h"
#include "GPU/Common/TextureReplacer.h"
#include "GPU/Common/TextureDecoder.h"

static const std::string INI_FILENAME = "textures.ini";
static const std::string ZIP_FILENAME = "textures.zip";
static const std::string NEW_TEXTURE_DIR = "new/";
static const int VERSION = 1;
static const double MAX_CACHE_SIZE = 4.0;
static bool basisu_initialized = false;

TextureReplacer::TextureReplacer(Draw::DrawContext *draw) {
	if (!basisu_initialized) {
		basist::basisu_transcoder_init();
		basisu_initialized = true;
	}
	// We don't want to keep the draw object around, so extract the info we need.
	if (draw->GetDataFormatSupport(Draw::DataFormat::BC3_UNORM_BLOCK)) formatSupport_.bc123 = true;
	if (draw->GetDataFormatSupport(Draw::DataFormat::ASTC_4x4_UNORM_BLOCK)) formatSupport_.astc = true;
	if (draw->GetDataFormatSupport(Draw::DataFormat::BC7_UNORM_BLOCK)) formatSupport_.bc7 = true;
	if (draw->GetDataFormatSupport(Draw::DataFormat::ETC2_R8G8B8_UNORM_BLOCK)) formatSupport_.etc2 = true;
}

TextureReplacer::~TextureReplacer() {
	for (auto iter : levelCache_) {
		delete iter.second;
	}
	delete vfs_;
}

void TextureReplacer::Init() {
	NotifyConfigChanged();
}

void TextureReplacer::NotifyConfigChanged() {
	gameID_ = g_paramSFO.GetDiscID();

	bool wasEnabled = enabled_;
	enabled_ = g_Config.bReplaceTextures || g_Config.bSaveNewTextures;
	if (enabled_) {
		basePath_ = GetSysDirectory(DIRECTORY_TEXTURES) / gameID_;

		newTextureDir_ = basePath_ / NEW_TEXTURE_DIR;

		// If we're saving, auto-create the directory.
		if (g_Config.bSaveNewTextures && !File::Exists(newTextureDir_)) {
			File::CreateFullPath(newTextureDir_);
			File::CreateEmptyFile(newTextureDir_ / ".nomedia");
		}

		enabled_ = File::IsDirectory(basePath_);
	} else if (wasEnabled) {
		delete vfs_;
		vfs_ = nullptr;
		Decimate(ReplacerDecimateMode::ALL);
	}

	if (enabled_) {
		enabled_ = LoadIni();
	}
}

bool TextureReplacer::LoadIni() {
	hash_ = ReplacedTextureHash::QUICK;
	aliases_.clear();
	hashranges_.clear();
	filtering_.clear();
	reducehashranges_.clear();

	allowVideo_ = false;
	ignoreAddress_ = false;
	reduceHash_ = false;
	reduceHashGlobalValue = 0.5;
	// Prevents dumping the mipmaps.
	ignoreMipmap_ = false;

	delete vfs_;
	vfs_ = nullptr;

	Path zipPath = basePath_ / ZIP_FILENAME;

	// First, check for textures.zip, which is used to reduce IO.
	VFSBackend *dir = ZipFileReader::Create(zipPath, "", false);
	if (!dir) {
		INFO_LOG(G3D, "%s wasn't a zip file - opening the directory %s instead.", zipPath.c_str(), basePath_.c_str());
		vfsIsZip_ = false;
		dir = new DirectoryReader(basePath_);
	} else {
		vfsIsZip_ = true;
	}

	IniFile ini;
	bool iniLoaded = ini.LoadFromVFS(*dir, INI_FILENAME);

	if (iniLoaded) {
		if (!LoadIniValues(ini)) {
			delete dir;
			return false;
		}

		// Allow overriding settings per game id.
		std::string overrideFilename;
		if (ini.GetOrCreateSection("games")->Get(gameID_.c_str(), &overrideFilename, "")) {
			if (!overrideFilename.empty() && overrideFilename != INI_FILENAME) {
				IniFile overrideIni;
				iniLoaded = overrideIni.LoadFromVFS(*dir, overrideFilename);
				if (!iniLoaded) {
					ERROR_LOG(G3D, "Failed to load extra texture ini: %s", overrideFilename.c_str());
					// Since this error is most likely to occure for texture pack creators, let's just bail here
					// so that the creator is more likely to look in the logs for what happened.
					delete dir;
					return false;
				}

				INFO_LOG(G3D, "Loading extra texture ini: %s", overrideFilename.c_str());
				if (!LoadIniValues(overrideIni, true)) {
					delete dir;
					return false;
				}
			}
		}
	} else {
		if (vfsIsZip_) {
			// We don't accept zip files without inis.
			ERROR_LOG(G3D, "Texture pack lacking ini file: %s", basePath_.c_str());
			delete dir;
			return false;
		} else {
			WARN_LOG(G3D, "Texture pack lacking ini file: %s", basePath_.c_str());
		}
	}

	vfs_ = dir;

	// If we have stuff loaded from before, need to update the vfs pointers to avoid
	// crash on exit. The actual problem is that we tend to call LoadIni a little too much...
	for (auto &repl : levelCache_) {
		repl.second->vfs_ = vfs_;
	}

	if (vfsIsZip_) {
		INFO_LOG(G3D, "Texture pack activated from '%s'", (basePath_ / ZIP_FILENAME).c_str());
	} else {
		INFO_LOG(G3D, "Texture pack activated from '%s'", basePath_.c_str());
	}

	// The ini doesn't have to exist for the texture directory or zip to be valid.
	return true;
}

bool TextureReplacer::LoadIniValues(IniFile &ini, bool isOverride) {
	auto options = ini.GetOrCreateSection("options");
	std::string hash;
	options->Get("hash", &hash, "");
	if (strcasecmp(hash.c_str(), "quick") == 0) {
		hash_ = ReplacedTextureHash::QUICK;
	} else if (strcasecmp(hash.c_str(), "xxh32") == 0) {
		hash_ = ReplacedTextureHash::XXH32;
	} else if (strcasecmp(hash.c_str(), "xxh64") == 0) {
		hash_ = ReplacedTextureHash::XXH64;
	} else if (!isOverride || !hash.empty()) {
		ERROR_LOG(G3D, "Unsupported hash type: %s", hash.c_str());
		return false;
	}

	options->Get("video", &allowVideo_, allowVideo_);
	options->Get("ignoreAddress", &ignoreAddress_, ignoreAddress_);
	// Multiplies sizeInRAM/bytesPerLine in XXHASH by 0.5.
	options->Get("reduceHash", &reduceHash_, reduceHash_);
	options->Get("ignoreMipmap", &ignoreMipmap_, ignoreMipmap_);
	if (reduceHash_ && hash_ == ReplacedTextureHash::QUICK) {
		reduceHash_ = false;
		ERROR_LOG(G3D, "Texture Replacement: reduceHash option requires safer hash, use xxh32 or xxh64 instead.");
	}

	if (ignoreAddress_ && hash_ == ReplacedTextureHash::QUICK) {
		ignoreAddress_ = false;
		ERROR_LOG(G3D, "Texture Replacement: ignoreAddress option requires safer hash, use xxh32 or xxh64 instead.");
	}

	int version = 0;
	if (options->Get("version", &version, 0) && version > VERSION) {
		ERROR_LOG(G3D, "Unsupported texture replacement version %d, trying anyway", version);
	}

	bool filenameWarning = false;
	if (ini.HasSection("hashes")) {
		auto hashes = ini.GetOrCreateSection("hashes")->ToMap();
		// Format: hashname = filename.png
		bool checkFilenames = g_Config.bSaveNewTextures && !g_Config.bIgnoreTextureFilenames && !vfsIsZip_;

		std::map<ReplacementCacheKey, std::map<int, std::string>> filenameMap;

		for (const auto &item : hashes) {
			ReplacementCacheKey key(0, 0);
			int level = 0;  // sscanf might fail to pluck the level, but that's ok, we default to 0.
			if (sscanf(item.first.c_str(), "%16llx%8x_%d", &key.cachekey, &key.hash, &level) >= 1) {
				filenameMap[key][level] = item.second;
				if (checkFilenames) {
#if PPSSPP_PLATFORM(WINDOWS)
					// Uppercase probably means the filenames don't match.
					// Avoiding an actual check of the filenames to avoid performance impact.
					filenameWarning = filenameWarning || item.second.find_first_of("\\ABCDEFGHIJKLMNOPQRSTUVWXYZ:<>|?*") != std::string::npos;
#else
					filenameWarning = filenameWarning || item.second.find_first_of("\\:<>|?*") != std::string::npos;
#endif
				}
			} else {
				ERROR_LOG(G3D, "Unsupported syntax under [hashes]: %s", item.first.c_str());
			}
		}

		// Now, translate the filenameMap to the final aliasMap.
		for (auto &pair : filenameMap) {
			std::string alias;
			int mipIndex = 0;
			for (auto &level : pair.second) {
				if (level.first == mipIndex) {
					alias += level.second + "|";
					mipIndex++;
				} else {
					WARN_LOG(G3D, "Non-sequential mip index %d, breaking. filenames=%s", level.first, level.second.c_str());
					break;
				}
			}
			if (alias == "|") {
				alias = "";  // marker for no replacement
			}
			aliases_[pair.first] = alias;
		}
	}

	if (filenameWarning) {
		auto err = GetI18NCategory("Error");
		host->NotifyUserMessage(err->T("textures.ini filenames may not be cross-platform (banned characters)"), 6.0f);
	}

	if (ini.HasSection("hashranges")) {
		auto hashranges = ini.GetOrCreateSection("hashranges")->ToMap();
		// Format: addr,w,h = newW,newH
		for (const auto &item : hashranges) {
			ParseHashRange(item.first, item.second);
		}
	}

	if (ini.HasSection("filtering")) {
		auto filters = ini.GetOrCreateSection("filtering")->ToMap();
		// Format: hashname = nearest or linear
		for (const auto &item : filters) {
			ParseFiltering(item.first, item.second);
		}
	}

	if (ini.HasSection("reducehashranges")) {
		auto reducehashranges = ini.GetOrCreateSection("reducehashranges")->ToMap();
		// Format: w,h = reducehashvalues
		for (const auto& item : reducehashranges) {
			ParseReduceHashRange(item.first, item.second);
		}
	}

	return true;
}

void TextureReplacer::ParseHashRange(const std::string &key, const std::string &value) {
	std::vector<std::string> keyParts;
	SplitString(key, ',', keyParts);
	std::vector<std::string> valueParts;
	SplitString(value, ',', valueParts);

	if (keyParts.size() != 3 || valueParts.size() != 2) {
		ERROR_LOG(G3D, "Ignoring invalid hashrange %s = %s, expecting addr,w,h = w,h", key.c_str(), value.c_str());
		return;
	}

	u32 addr;
	u32 fromW;
	u32 fromH;
	if (!TryParse(keyParts[0], &addr) || !TryParse(keyParts[1], &fromW) || !TryParse(keyParts[2], &fromH)) {
		ERROR_LOG(G3D, "Ignoring invalid hashrange %s = %s, key format is 0x12345678,512,512", key.c_str(), value.c_str());
		return;
	}

	u32 toW;
	u32 toH;
	if (!TryParse(valueParts[0], &toW) || !TryParse(valueParts[1], &toH)) {
		ERROR_LOG(G3D, "Ignoring invalid hashrange %s = %s, value format is 512,512", key.c_str(), value.c_str());
		return;
	}

	if (toW > fromW || toH > fromH) {
		ERROR_LOG(G3D, "Ignoring invalid hashrange %s = %s, range bigger than source", key.c_str(), value.c_str());
		return;
	}

	const u64 rangeKey = ((u64)addr << 32) | ((u64)fromW << 16) | fromH;
	hashranges_[rangeKey] = WidthHeightPair(toW, toH);
}

void TextureReplacer::ParseFiltering(const std::string &key, const std::string &value) {
	ReplacementCacheKey itemKey(0, 0);
	if (sscanf(key.c_str(), "%16llx%8x", &itemKey.cachekey, &itemKey.hash) >= 1) {
		if (!strcasecmp(value.c_str(), "nearest")) {
			filtering_[itemKey] = TEX_FILTER_FORCE_NEAREST;
		} else if (!strcasecmp(value.c_str(), "linear")) {
			filtering_[itemKey] = TEX_FILTER_FORCE_LINEAR;
		} else if (!strcasecmp(value.c_str(), "auto")) {
			filtering_[itemKey] = TEX_FILTER_AUTO;
		} else {
			ERROR_LOG(G3D, "Unsupported syntax under [filtering]: %s", value.c_str());
		}
	} else {
		ERROR_LOG(G3D, "Unsupported syntax under [filtering]: %s", key.c_str());
	}
}

void TextureReplacer::ParseReduceHashRange(const std::string& key, const std::string& value) {
	std::vector<std::string> keyParts;
	SplitString(key, ',', keyParts);
	std::vector<std::string> valueParts;
	SplitString(value, ',', valueParts);

	if (keyParts.size() != 2 || valueParts.size() != 1) {
		ERROR_LOG(G3D, "Ignoring invalid reducehashrange %s = %s, expecting w,h = reducehashvalue", key.c_str(), value.c_str());
		return;
	}

	u32 forW;
	u32 forH;
	if (!TryParse(keyParts[0], &forW) || !TryParse(keyParts[1], &forH)) {
		ERROR_LOG(G3D, "Ignoring invalid reducehashrange %s = %s, key format is 512,512", key.c_str(), value.c_str());
		return;
	}

	float rhashvalue;
	if (!TryParse(valueParts[0], &rhashvalue)) {
		ERROR_LOG(G3D, "Ignoring invalid reducehashrange %s = %s, value format is 0.5", key.c_str(), value.c_str());
		return;
	}

	if (rhashvalue == 0) {
		ERROR_LOG(G3D, "Ignoring invalid hashrange %s = %s, reducehashvalue can't be 0", key.c_str(), value.c_str());
		return;
	}

	const u64 reducerangeKey = ((u64)forW << 16) | forH;
	reducehashranges_[reducerangeKey] = rhashvalue;
}

u32 TextureReplacer::ComputeHash(u32 addr, int bufw, int w, int h, GETextureFormat fmt, u16 maxSeenV) {
	_dbg_assert_msg_(enabled_, "Replacement not enabled");

	if (!LookupHashRange(addr, w, h, &w, &h)) {
		// There wasn't any hash range, let's fall back to maxSeenV logic.
		if (h == 512 && maxSeenV < 512 && maxSeenV != 0) {
			h = (int)maxSeenV;
		}
	}

	const u8 *checkp = Memory::GetPointerUnchecked(addr);
	if (reduceHash_) {
		reduceHashSize = LookupReduceHashRange(w, h);
		// default to reduceHashGlobalValue which default is 0.5
	}
	if (bufw <= w) {
		// We can assume the data is contiguous.  These are the total used pixels.
		const u32 totalPixels = bufw * h + (w - bufw);
		const u32 sizeInRAM = (textureBitsPerPixel[fmt] * totalPixels) / 8 * reduceHashSize;

		switch (hash_) {
		case ReplacedTextureHash::QUICK:
			return StableQuickTexHash(checkp, sizeInRAM);
		case ReplacedTextureHash::XXH32:
			return XXH32(checkp, sizeInRAM, 0xBACD7814);
		case ReplacedTextureHash::XXH64:
			return XXH64(checkp, sizeInRAM, 0xBACD7814);
		default:
			return 0;
		}
	} else {
		// We have gaps.  Let's hash each row and sum.
		const u32 bytesPerLine = (textureBitsPerPixel[fmt] * w) / 8 * reduceHashSize;
		const u32 stride = (textureBitsPerPixel[fmt] * bufw) / 8;

		u32 result = 0;
		switch (hash_) {
		case ReplacedTextureHash::QUICK:
			for (int y = 0; y < h; ++y) {
				u32 rowHash = StableQuickTexHash(checkp, bytesPerLine);
				result = (result * 11) ^ rowHash;
				checkp += stride;
			}
			break;

		case ReplacedTextureHash::XXH32:
			for (int y = 0; y < h; ++y) {
				u32 rowHash = XXH32(checkp, bytesPerLine, 0xBACD7814);
				result = (result * 11) ^ rowHash;
				checkp += stride;
			}
			break;

		case ReplacedTextureHash::XXH64:
			for (int y = 0; y < h; ++y) {
				u32 rowHash = XXH64(checkp, bytesPerLine, 0xBACD7814);
				result = (result * 11) ^ rowHash;
				checkp += stride;
			}
			break;

		default:
			break;
		}

		return result;
	}
}

ReplacedTexture *TextureReplacer::FindReplacement(u64 cachekey, u32 hash, int w, int h) {
	// Only actually replace if we're replacing.  We might just be saving.
	if (!Enabled() || !g_Config.bReplaceTextures) {
		return nullptr;
	}

	ReplacementCacheKey replacementKey(cachekey, hash);
	auto it = cache_.find(replacementKey);
	if (it != cache_.end()) {
		return it->second.texture;
	}

	ReplacementDesc desc;
	desc.newW = w;
	desc.newH = h;
	desc.w = w;
	desc.h = h;
	desc.cachekey = cachekey;
	desc.hash = hash;
	LookupHashRange(cachekey >> 32, w, h, &desc.newW, &desc.newH);

	if (ignoreAddress_) {
		cachekey = cachekey & 0xFFFFFFFFULL;
	}

	bool foundAlias = false;
	bool ignored = false;
	std::string hashfiles = LookupHashFile(cachekey, hash, &foundAlias, &ignored);

	// Early-out for ignored textures, let's not bother even starting a thread task.
	if (ignored) {
		// WARN_LOG(G3D, "Not found/ignored: %s (%d, %d)", hashfiles.c_str(), (int)foundReplacement, (int)ignored);
		// Insert an entry into the cache for faster lookup next time.
		ReplacedTextureRef ref{};
		cache_.emplace(std::make_pair(replacementKey, ref));
		return nullptr;
	}

	desc.forceFiltering = (TextureFiltering)0;  // invalid value
	FindFiltering(cachekey, hash, &desc.forceFiltering);

	if (!foundAlias) {
		// We'll just need to generate the names for each level.
		// By default, we look for png since that's also what's dumped.
		// For other file formats, use the ini to create aliases.
		desc.filenames.resize(MAX_REPLACEMENT_MIP_LEVELS);
		for (int level = 0; level < desc.filenames.size(); level++) {
			desc.filenames[level] = TextureReplacer::HashName(cachekey, hash, level) + ".png";
		}
		desc.logId = desc.filenames[0];
		desc.hashfiles = desc.filenames[0];  // The generated filename of the top level is used as the key in the data cache.
	} else {
		desc.logId = hashfiles;
		SplitString(hashfiles, '|', desc.filenames);
		desc.hashfiles = hashfiles;
	}

	// OK, we might already have a matching texture, we use hashfiles as a key. Look it up in the level cache.
	auto iter = levelCache_.find(hashfiles);
	if (iter != levelCache_.end()) {
		// Insert an entry into the cache for faster lookup next time.
		ReplacedTextureRef ref;
		ref.hashfiles = hashfiles;
		ref.texture = iter->second;
		cache_.emplace(std::make_pair(replacementKey, ref));
		return iter->second;
	}

	// Final path - we actually need a new replacement texture, because we haven't seen "hashfiles" before.
	desc.basePath = basePath_;
	desc.formatSupport = formatSupport_;

	ReplacedTexture *texture = new ReplacedTexture(vfs_, desc);

	ReplacedTextureRef ref;
	ref.hashfiles = hashfiles;
	ref.texture = texture;
	cache_.emplace(std::make_pair(replacementKey, ref));

	// Also, insert the level in the level cache so we can look up by desc_->hashfiles again.
	levelCache_.emplace(std::make_pair(hashfiles, texture));
	return texture;
}

static bool WriteTextureToPNG(png_imagep image, const Path &filename, int convert_to_8bit, const void *buffer, png_int_32 row_stride, const void *colormap) {
	FILE *fp = File::OpenCFile(filename, "wb");
	if (!fp) {
		ERROR_LOG(IO, "Unable to open texture file for writing.");
		return false;
	}

	if (png_image_write_to_stdio(image, fp, convert_to_8bit, buffer, row_stride, colormap)) {
		fclose(fp);
		return true;
	} else {
		ERROR_LOG(SYSTEM, "Texture PNG encode failed.");
		fclose(fp);
		remove(filename.c_str());
		return false;
	}
}

// We save textures on threadpool tasks since it's a fire-and-forget task, and both I/O and png compression
// can be pretty slow.
class SaveTextureTask : public Task {
public:
	std::vector<u8> rgbaData;

	int w = 0;
	int h = 0;
	int pitch = 0;  // bytes

	Path filename;
	Path saveFilename;
	bool createSaveDirectory = false;
	Path saveDirectory;

	u32 replacedInfoHash = 0;

	bool skipIfExists = false;

	SaveTextureTask(std::vector<u8> &&_rgbaData) : rgbaData(std::move(_rgbaData)) {}

	// This must be set to I/O blocking because of Android storage (so we attach the thread to JNI), while being CPU heavy too.
	TaskType Type() const override { return TaskType::IO_BLOCKING; }

	TaskPriority Priority() const override {
		return TaskPriority::LOW;
	}

	void Run() override {
		// Should we skip writing if the newly saved data already exists?
		if (skipIfExists && File::Exists(saveFilename)) {
			return;
		}

		// And we always skip if the replace file already exists.
		if (File::Exists(filename))
			return;

		if (createSaveDirectory && !File::Exists(saveDirectory)) {
			File::CreateFullPath(saveDirectory);
			File::CreateEmptyFile(saveDirectory / ".nomedia");
		}

		png_image png{};
		png.version = PNG_IMAGE_VERSION;
		png.format = PNG_FORMAT_RGBA;
		png.width = w;
		png.height = h;
		bool success = WriteTextureToPNG(&png, saveFilename, 0, rgbaData.data(), pitch, nullptr);
		png_image_free(&png);
		if (png.warning_or_error >= 2) {
			ERROR_LOG(G3D, "Saving screenshot to PNG produced errors.");
		} else if (success) {
			NOTICE_LOG(G3D, "Saving texture for replacement: %08x / %dx%d in '%s'", replacedInfoHash, w, h, saveFilename.ToVisualString().c_str());
		} else {
			ERROR_LOG(G3D, "Failed to write '%s'", saveFilename.c_str());
		}
	}
};

bool TextureReplacer::WillSave(const ReplacedTextureDecodeInfo &replacedInfo) {
	_assert_msg_(enabled_, "Replacement not enabled");
	if (!g_Config.bSaveNewTextures)
		return false;
	// Don't save the PPGe texture.
	if (replacedInfo.addr > 0x05000000 && replacedInfo.addr < PSP_GetKernelMemoryEnd())
		return false;
	if (replacedInfo.isVideo && !allowVideo_)
		return false;

	return true;
}

void TextureReplacer::NotifyTextureDecoded(const ReplacedTextureDecodeInfo &replacedInfo, const void *data, int pitch, int level, int origW, int origH, int scaledW, int scaledH) {
	_assert_msg_(enabled_, "Replacement not enabled");
	if (!WillSave(replacedInfo)) {
		// Ignore.
		return;
	}
	if (ignoreMipmap_ && level > 0) {
		return;
	}

	u64 cachekey = replacedInfo.cachekey;
	if (ignoreAddress_) {
		cachekey = cachekey & 0xFFFFFFFFULL;
	}

	bool foundAlias = false, ignored = false;
	std::string hashfile = LookupHashFile(cachekey, replacedInfo.hash, &foundAlias, &ignored);

	// If it's empty, it's an ignored hash, we intentionally don't save.
	if (foundAlias || ignored) {
		// If it exists, must've been decoded and saved as a new texture already.
		return;
	}

	// Generate a new PNG filename, complete with level.
	hashfile = HashName(cachekey, replacedInfo.hash, level) + ".png";

	ReplacementCacheKey replacementKey(cachekey, replacedInfo.hash);
	auto it = savedCache_.find(replacementKey);
	bool skipIfExists = false;
	double now = time_now_d();
	if (it != savedCache_.end()) {
		// We've already saved this texture. Ignore it.
		// We don't really care about changing the scale factor during runtime, only confusing.
		return;
	}

	// Width/height of the image to save.
	int w = scaledW;
	int h = scaledH;

	// Only save the hashed portion of the PNG.
	int lookupW;
	int lookupH;
	if (LookupHashRange(replacedInfo.addr, origW, origH, &lookupW, &lookupH)) {
		w = lookupW * (scaledW / origW);
		h = lookupH * (scaledH / origH);
	}

	std::vector<u8> saveBuf;

	// Copy data to a buffer so we can send it to the thread. Might as well compact-away the pitch
	// while we're at it.
	saveBuf.resize(w * h * 4);
	for (int y = 0; y < h; y++) {
		memcpy((u8 *)saveBuf.data() + y * w * 4, (const u8 *)data + y * pitch, w * sizeof(u32));
	}
	pitch = w * 4;

	SaveTextureTask *task = new SaveTextureTask(std::move(saveBuf));

	task->filename = basePath_ / hashfile;
	task->saveFilename = newTextureDir_ / hashfile;
	task->createSaveDirectory = false;

	// Create subfolder as needed.
#ifdef _WIN32
	size_t slash = hashfile.find_last_of("/\\");
#else
	size_t slash = hashfile.find_last_of("/");
#endif
	if (slash != hashfile.npos) {
		// Does this ever happen?
		// Answer: An alias could be used to save a texture in a subfolder of newTextureDir_
		// (i.e. if you had the hash and purged out your pngs to redump them), although I guess this usage is probably uncommon.  - unknown

		// Create any directory structure as needed.
		task->saveDirectory = newTextureDir_ / hashfile.substr(0, slash);
		task->createSaveDirectory = true;
	}

	task->w = w;
	task->h = h;
	task->pitch = pitch;
	task->replacedInfoHash = replacedInfo.hash;
	task->skipIfExists = skipIfExists;
	g_threadManager.EnqueueTask(task);  // We don't care about waiting for the task. It'll be fine.

	// Remember that we've saved this for next time.
	// Should be OK that the actual disk write may not be finished yet.
	SavedTextureCacheData &saveData = savedCache_[replacementKey];
	saveData.levelW[level] = w;
	saveData.levelH[level] = h;
	saveData.levelSaved[level] = true;
	saveData.lastTimeSaved = now;
}

void TextureReplacer::Decimate(ReplacerDecimateMode mode) {
	// Allow replacements to be cached for a long time, although they're large.
	double age = 1800.0;
	if (mode == ReplacerDecimateMode::FORCE_PRESSURE) {
		age = 90.0;
	} else if (mode == ReplacerDecimateMode::ALL) {
		age = 0.0;
	} else if (lastTextureCacheSizeGB_ > 1.0) {
		double pressure = std::min(MAX_CACHE_SIZE, lastTextureCacheSizeGB_) / MAX_CACHE_SIZE;
		// Get more aggressive the closer we are to the max.
		age = 90.0 + (1.0 - pressure) * 1710.0;
	}

	const double threshold = time_now_d() - age;
	size_t totalSize = 0;
	for (auto &item : levelCache_) {
		std::lock_guard<std::mutex> guard(item.second->lock_);
		item.second->PurgeIfNotUsedSinceTime(threshold);
		totalSize += item.second->GetTotalDataSize();  // TODO: Make something better.
		// don't actually delete the items here, just clean out the data.
	}

	double totalSizeGB = totalSize / (1024.0 * 1024.0 * 1024.0);
	if (totalSizeGB >= 1.0) {
		WARN_LOG(G3D, "Decimated replacements older than %fs, currently using %f GB of RAM", age, totalSizeGB);
	}
	lastTextureCacheSizeGB_ = totalSizeGB;
}

template <typename Key, typename Value>
static typename std::unordered_map<Key, Value>::const_iterator LookupWildcard(const std::unordered_map<Key, Value> &map, Key &key, u64 cachekey, u32 hash, bool ignoreAddress) {
	auto alias = map.find(key);
	if (alias != map.end())
		return alias;

	// Also check for a few more aliases with zeroed portions:
	// Only clut hash (very dangerous in theory, in practice not more than missing "just" data hash)
	key.cachekey = cachekey & 0xFFFFFFFFULL;
	key.hash = 0;
	alias = map.find(key);
	if (alias != map.end())
		return alias;

	if (!ignoreAddress) {
		// No data hash.
		key.cachekey = cachekey;
		key.hash = 0;
		alias = map.find(key);
		if (alias != map.end())
			return alias;
	}

	// No address.
	key.cachekey = cachekey & 0xFFFFFFFFULL;
	key.hash = hash;
	alias = map.find(key);
	if (alias != map.end())
		return alias;

	if (!ignoreAddress) {
		// Address, but not clut hash (in case of garbage clut data.)
		key.cachekey = cachekey & ~0xFFFFFFFFULL;
		key.hash = hash;
		alias = map.find(key);
		if (alias != map.end())
			return alias;
	}

	// Anything with this data hash (a little dangerous.)
	key.cachekey = 0;
	key.hash = hash;
	return map.find(key);
}

bool TextureReplacer::FindFiltering(u64 cachekey, u32 hash, TextureFiltering *forceFiltering) {
	if (!Enabled() || !g_Config.bReplaceTextures) {
		return false;
	}

	ReplacementCacheKey replacementKey(cachekey, hash);
	auto filter = LookupWildcard(filtering_, replacementKey, cachekey, hash, ignoreAddress_);
	if (filter == filtering_.end()) {
		// Allow a global wildcard.
		replacementKey.cachekey = 0;
		replacementKey.hash = 0;
		filter = filtering_.find(replacementKey);
	}
	if (filter != filtering_.end()) {
		*forceFiltering = filter->second;
		return true;
	}
	return false;
}

std::string TextureReplacer::LookupHashFile(u64 cachekey, u32 hash, bool *foundAlias, bool *ignored) {
	ReplacementCacheKey key(cachekey, hash);
	auto alias = LookupWildcard(aliases_, key, cachekey, hash, ignoreAddress_);
	if (alias != aliases_.end()) {
		// Note: this will be blank if explicitly ignored.
		*foundAlias = true;
		*ignored = alias->second.empty();
		return alias->second;
	}
	*foundAlias = false;
	*ignored = false;
	return "";
}

std::string TextureReplacer::HashName(u64 cachekey, u32 hash, int level) {
	char hashname[16 + 8 + 1 + 11 + 1] = {};
	if (level > 0) {
		snprintf(hashname, sizeof(hashname), "%016llx%08x_%d", cachekey, hash, level);
	} else {
		snprintf(hashname, sizeof(hashname), "%016llx%08x", cachekey, hash);
	}

	return hashname;
}

bool TextureReplacer::LookupHashRange(u32 addr, int w, int h, int *newW, int *newH) {
	const u64 rangeKey = ((u64)addr << 32) | ((u64)w << 16) | h;
	auto range = hashranges_.find(rangeKey);
	if (range != hashranges_.end()) {
		const WidthHeightPair &wh = range->second;
		*newW = wh.first;
		*newH = wh.second;
		return true;
	} else {
		*newW = w;
		*newH = h;
		return false;
	}
}

float TextureReplacer::LookupReduceHashRange(int& w, int& h) {
	const u64 reducerangeKey = ((u64)w << 16) | h;
	auto range = reducehashranges_.find(reducerangeKey);
	if (range != reducehashranges_.end()) {
		float rhv = range->second;
		return rhv;
	}
	else {
		return reduceHashGlobalValue;
	}
}

bool TextureReplacer::IniExists(const std::string &gameID) {
	if (gameID.empty())
		return false;

	Path texturesDirectory = GetSysDirectory(DIRECTORY_TEXTURES) / gameID;
	Path generatedFilename = texturesDirectory / INI_FILENAME;
	return File::Exists(generatedFilename);
}

bool TextureReplacer::GenerateIni(const std::string &gameID, Path &generatedFilename) {
	if (gameID.empty())
		return false;

	Path texturesDirectory = GetSysDirectory(DIRECTORY_TEXTURES) / gameID;
	if (!File::Exists(texturesDirectory)) {
		File::CreateFullPath(texturesDirectory);
	}

	generatedFilename = texturesDirectory / INI_FILENAME;
	if (File::Exists(generatedFilename))
		return true;

	FILE *f = File::OpenCFile(generatedFilename, "wb");
	if (f) {
		// Unicode byte order mark
		fwrite("\xEF\xBB\xBF", 1, 3, f);

		// Let's also write some defaults.
		fprintf(f, R"(# This describes your textures and set up options for texture replacement.
# Documentation about the options and syntax is available here:
# https://www.ppsspp.org/docs/reference/texture-replacement

[options]
version = 1
hash = quick
ignoreMipmap = true  # Set to true to avoid dumping mipmaps. Instead use basisu to generate them, see docs.
reduceHash = false
allowVideo = false

[games]
# Used to make it easier to install, and override settings for other regions.
# Files still have to be copied to each TEXTURES folder.
%s = %s

[hashes]
# Use / for folders not \\, avoid special characters, and stick to lowercase.
# See wiki for more info.

[hashranges]
# See the documentation.
# Example: 08b31020,512,512 = 480,272

[filtering]
# You can enforce specific filtering modes with this. Available modes are linear/nearest/auto. See the docs.
# Example: 08d3961000000909ba70b2af = linear

[reducehashranges]
# Lets you set regions of memory where reduced hashing applies. See the docs.
)", gameID.c_str(), INI_FILENAME.c_str());
		fclose(f);
	}
	return File::Exists(generatedFilename);
}
