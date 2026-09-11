#include "gepch.h"
#include "TextureImporter.h"

#include "GanymedE/Assets/AssetManager.h"
#include "GanymedE/Assets/AssetPaths.h"
#include "GanymedE/Renderer/Texture.h"

#include <stb_image.h>

namespace GanymedE {

	void DecodedImage::PixelDeleter::operator()(uint8_t* pixels) const
	{
		stbi_image_free(pixels);
	}

	namespace {

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

	namespace {

		// stb's flip flag is a **process-wide global** in the vendored copy - it has no
		// STBI_THREAD_LOCAL support - and decoding runs on worker threads now: a texture compile
		// (flip off) and a mesh's embedded image (flip on) can be in flight at the same instant.
		// Writing that global from a worker is a data race whose symptom is an occasionally
		// upside-down texture, which is about the worst kind of bug to go looking for.
		//
		// So the global is never written at all. stb decodes in its default orientation and the
		// flip, when asked for, happens here on a buffer this thread owns. One pass of row swaps
		// against a decode that costs milliseconds.
		void FlipRows(uint8_t* pixels, uint32_t width, uint32_t height)
		{
			const std::size_t stride = (std::size_t)width * 4;
			std::vector<uint8_t> scratch(stride);

			for (uint32_t y = 0; y < height / 2; y++)
			{
				uint8_t* top = pixels + (std::size_t)y * stride;
				uint8_t* bottom = pixels + (std::size_t)(height - 1 - y) * stride;

				std::memcpy(scratch.data(), top, stride);
				std::memcpy(top, bottom, stride);
				std::memcpy(bottom, scratch.data(), stride);
			}
		}

	}

	DecodedImage TextureImporter::Decode(const std::filesystem::path& fullPath, bool flipVertically)
	{
		GE_PROFILE_FUNCTION();

		int width, height, channels;
		// Force 4 channels: bgfx has no 24-bit RGB8 format, and asking stb to
		// expand avoids a manual repack.
		unsigned char* pixels = stbi_load(fullPath.string().c_str(), &width, &height, &channels, 4);
		if (!pixels)
		{
			GE_CORE_ERROR("Failed to load image '{0}'", fullPath.string());
			return {};
		}

		if (flipVertically)
			FlipRows(pixels, (uint32_t)width, (uint32_t)height);

		return { std::unique_ptr<uint8_t, DecodedImage::PixelDeleter>(pixels),
			(uint32_t)width, (uint32_t)height };
	}

	DecodedImage TextureImporter::DecodeFromMemory(const uint8_t* bytes, size_t size, bool flipVertically)
	{
		GE_PROFILE_FUNCTION();

		if (!bytes || size == 0)
			return {};

		int width, height, channels;
		unsigned char* pixels = stbi_load_from_memory(bytes, (int)size, &width, &height, &channels, 4);
		if (!pixels)
		{
			GE_CORE_ERROR("Failed to decode embedded image ({0} bytes)", size);
			return {};
		}

		if (flipVertically)
			FlipRows(pixels, (uint32_t)width, (uint32_t)height);

		return { std::unique_ptr<uint8_t, DecodedImage::PixelDeleter>(pixels),
			(uint32_t)width, (uint32_t)height };
	}

	Ref<Texture2D> TextureImporter::Upload(const DecodedImage& image)
	{
		if (!image)
			return nullptr;

		Ref<Texture2D> texture = Texture2D::Create(image.Width, image.Height);
		texture->SetData(image.Pixels.get(), image.Width * image.Height * 4);
		return texture;
	}

	Ref<Texture2D> TextureImporter::LoadFromFile(const std::filesystem::path& fullPath, bool flipVertically)
	{
		return Upload(Decode(fullPath, flipVertically));
	}

	Ref<Texture2D> TextureImporter::LoadFromMemory(const uint8_t* bytes, size_t size, bool flipVertically)
	{
		return Upload(DecodeFromMemory(bytes, size, flipVertically));
	}

	Ref<Texture2D> TextureImporter::LoadMaterialMap(const std::filesystem::path& relativePath,
		AssetHandle* outHandle)
	{
		if (outHandle)
			*outHandle = InvalidAssetHandle;

		if (relativePath.empty())
			return nullptr;

		if (IsInsideAssetRoot(relativePath))
		{
			AssetHandle handle = AssetManager::ImportAsset(relativePath);
			if (IsAssetHandleValid(handle))
			{
				if (outHandle)
					*outHandle = handle;

				return AssetManager::GetAsset<Texture2D>(handle);
			}
		}

		// operator/ replaces the left side when relativePath is absolute, which is the
		// shape MeshImporter records when relative() itself failed.
		return LoadFromFile(GetAssetRoot() / relativePath, false);
	}

}
