#include "gepch.h"
#include "TextureImporter.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Renderer/Texture.h"

#include <stb_image.h>

namespace GanymedE {

	namespace {

		// Takes ownership of stb's buffer.
		Ref<Texture2D> Upload(unsigned char* pixels, int width, int height)
		{
			Ref<Texture2D> texture = Texture2D::Create((uint32_t)width, (uint32_t)height);
			texture->SetData(pixels, (uint32_t)(width * height * 4));
			stbi_image_free(pixels);
			return texture;
		}

		// std::filesystem::relative returns "../../foo.png" with no error code for a
		// file outside the root, and such a path must never get a registry entry.
		bool IsInsideAssetRoot(const std::filesystem::path& relativePath)
		{
			if (relativePath.empty() || relativePath.is_absolute())
				return false;

			for (const auto& part : relativePath)
			{
				if (part == "..")
					return false;
			}
			return true;
		}

	}

	Ref<Texture2D> TextureImporter::LoadFromFile(const std::filesystem::path& fullPath, bool flipVertically)
	{
		GE_PROFILE_FUNCTION();

		stbi_set_flip_vertically_on_load(flipVertically ? 1 : 0);

		int width, height, channels;
		// Force 4 channels: bgfx has no 24-bit RGB8 format, and asking stb to
		// expand avoids a manual repack.
		unsigned char* pixels = stbi_load(fullPath.string().c_str(), &width, &height, &channels, 4);
		if (!pixels)
		{
			GE_CORE_ERROR("Failed to load image '{0}'", fullPath.string());
			return nullptr;
		}

		return Upload(pixels, width, height);
	}

	Ref<Texture2D> TextureImporter::LoadFromMemory(const uint8_t* bytes, size_t size, bool flipVertically)
	{
		GE_PROFILE_FUNCTION();

		if (!bytes || size == 0)
			return nullptr;

		stbi_set_flip_vertically_on_load(flipVertically ? 1 : 0);

		int width, height, channels;
		unsigned char* pixels = stbi_load_from_memory(bytes, (int)size, &width, &height, &channels, 4);
		if (!pixels)
		{
			GE_CORE_ERROR("Failed to decode embedded image ({0} bytes)", size);
			return nullptr;
		}

		return Upload(pixels, width, height);
	}

	Ref<Texture2D> TextureImporter::LoadMaterialMap(const std::filesystem::path& relativePath)
	{
		if (relativePath.empty())
			return nullptr;

		if (IsInsideAssetRoot(relativePath))
		{
			AssetHandle handle = AssetManager::ImportAsset(relativePath);
			if (IsAssetHandleValid(handle))
				return AssetManager::GetAsset<Texture2D>(handle);
		}

		// operator/ replaces the left side when relativePath is absolute, which is the
		// shape MeshImporter records when relative() itself failed.
		return LoadFromFile(GetAssetRoot() / relativePath, false);
	}

}
