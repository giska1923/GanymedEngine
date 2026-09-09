#include "TextureEncode.h"

#include <bimg/bimg.h>
#include <bimg/encode.h>
#include <bx/allocator.h>
#include <bx/error.h>
#include <bx/readerwriter.h>

#include <algorithm>

namespace GanymedE {

	namespace {

		// Stateless malloc/free wrapper, so one shared instance is safe on worker threads. bimg
		// allocates a scratch buffer inside each encode call and frees it before returning;
		// nothing is carried across calls.
		bx::DefaultAllocator s_Allocator;

		bimg::TextureFormat::Enum ToBimg(EncodedFormat format)
		{
			switch (format)
			{
				case EncodedFormat::BC1:   return bimg::TextureFormat::BC1;
				case EncodedFormat::BC3:   return bimg::TextureFormat::BC3;
				case EncodedFormat::BC4:   return bimg::TextureFormat::BC4;
				case EncodedFormat::BC5:   return bimg::TextureFormat::BC5;
				case EncodedFormat::BC7:   return bimg::TextureFormat::BC7;
				default:                   return bimg::TextureFormat::RGBA8;
			}
		}

		// Only libsquish's formats are safe to split across threads: it has static *functions*
		// and no mutable state, while AVPCL - the BC7 path - writes four file-scope bools per
		// block. See the note in the header.
		bool IsThreadSafeEncoder(bimg::TextureFormat::Enum format)
		{
			return format == bimg::TextureFormat::BC1
				|| format == bimg::TextureFormat::BC2
				|| format == bimg::TextureFormat::BC3
				|| format == bimg::TextureFormat::BC4
				|| format == bimg::TextureFormat::BC5;
		}

		// "Does this image use its alpha at all?" One linear pass, and it is what makes `Auto`
		// pick 4 bpp for the common case instead of paying 8 for an alpha channel that is 255
		// everywhere - which is most albedo maps, including every one shipped here.
		bool UsesAlpha(const uint8_t* rgba8, uint32_t width, uint32_t height)
		{
			const std::size_t count = (std::size_t)width * height;
			for (std::size_t i = 0; i < count; i++)
			{
				if (rgba8[i * 4 + 3] != 255)
					return true;
			}
			return false;
		}

		// One mip, encoded, split into horizontal bands so BC7 - seconds per 2K texture on one
		// thread - uses the machine.
		//
		// A band is safe to hand to another thread because the per-mip encode is a pure function
		// of (dst, src, width, height): rows of `blockHeight` pixels form independent block rows,
		// a band of full width and a block-multiple height is contiguous on both sides, and bimg
		// allocates its own scratch per call.
		void EncodeMip(const bimg::ImageMip& src, uint8_t* dst, uint32_t dstSize,
			bimg::TextureFormat::Enum dstFormat, bimg::Quality::Enum quality,
			const EncodeOptions& options)
		{
			const bimg::ImageBlockInfo& block = bimg::getBlockInfo(dstFormat);

			auto encodeRows = [&](uint32_t y, uint32_t height, std::size_t dstOffset)
			{
				bx::Error err;
				bimg::imageEncode(&s_Allocator, dst + dstOffset,
					src.m_data + (std::size_t)y * src.m_width * 4,
					bimg::TextureFormat::RGBA8, src.m_width, height, src.m_depth,
					dstFormat, quality, &err);
			};

			// Serial whenever the band arithmetic would not be exact. bimg derives its
			// destination row pitch as `width * bpp / 8`, which is only a whole number of blocks
			// when the width is - and the bottom mips of any chain are smaller than one block.
			// Encoding those in a single call is exactly what bimg's own imageEncode does, so
			// the output is byte-identical either way.
			const bool splittable = options.ParallelFor
				&& IsThreadSafeEncoder(dstFormat)
				&& block.blockWidth > 1
				&& src.m_width % block.blockWidth == 0
				&& src.m_height % block.blockHeight == 0
				&& src.m_height >= (uint32_t)block.blockHeight * 8;

			if (!splittable)
			{
				encodeRows(0, src.m_height, 0);
				return;
			}

			const uint32_t blocksPerRow = src.m_width / block.blockWidth;
			const uint32_t blockRows = src.m_height / block.blockHeight;

			// Four block rows per band: far past enkiTS's ~10k-cycle guidance for a grain (one
			// 1024-wide BC7 block row alone is orders of magnitude more), while still leaving
			// enough bands that the last worker to finish does not hold up the mip.
			constexpr uint32_t kBlockRowsPerBand = 4;
			const uint32_t bandCount = (blockRows + kBlockRowsPerBand - 1) / kBlockRowsPerBand;

			options.ParallelFor(bandCount, 1, [&](uint32_t begin, uint32_t end)
			{
				for (uint32_t band = begin; band < end; band++)
				{
					const uint32_t firstBlockRow = band * kBlockRowsPerBand;
					const uint32_t rowCount = std::min(kBlockRowsPerBand, blockRows - firstBlockRow);

					const std::size_t dstOffset =
						(std::size_t)firstBlockRow * blocksPerRow * block.blockSize;

					if (dstOffset + (std::size_t)rowCount * blocksPerRow * block.blockSize > dstSize)
						continue;

					encodeRows(firstBlockRow * block.blockHeight, rowCount * block.blockHeight, dstOffset);
				}
			});
		}

		// Where mip `count` starts, i.e. the size of the chain truncated to `count` mips.
		uint32_t MipChainSize(const bimg::ImageContainer& container, uint8_t count)
		{
			if (count >= container.m_numMips)
				return container.m_size;

			bimg::ImageMip mip;
			if (!bimg::imageGetRawData(container, 0, count, container.m_data, container.m_size, mip))
				return container.m_size;

			return (uint32_t)(mip.m_data - (const uint8_t*)container.m_data);
		}

	}

	bool EncodeTexture(const uint8_t* rgba8, uint32_t width, uint32_t height,
		const EncodeOptions& options, EncodeResult& result)
	{
		if (!rgba8 || width == 0 || height == 0)
		{
			result.Message = "no pixels";
			return false;
		}

		// imageAlloc copies, so the caller's buffer is free the moment this returns.
		bimg::ImageContainer* parsed = bimg::imageAlloc(&s_Allocator, bimg::TextureFormat::RGBA8,
			(uint16_t)width, (uint16_t)height, 1, 1, false, false, rgba8);

		if (!parsed)
		{
			result.Message = "the source image could not be allocated";
			return false;
		}

		EncodedFormat requested = options.Format;
		if (requested == EncodedFormat::Auto)
			requested = UsesAlpha(rgba8, width, height) ? EncodedFormat::BC3 : EncodedFormat::BC1;

		bimg::TextureFormat::Enum dstFormat = ToBimg(requested);
		bool compressed = requested != EncodedFormat::RGBA8;

		// Block formats need whole blocks. bimg's per-mip encode would write a short row for a
		// width that is not a multiple of the block width, producing a subtly corrupt texture
		// rather than failing - so fall back and say so instead. Real content is power-of-two.
		const bimg::ImageBlockInfo& block = bimg::getBlockInfo(dstFormat);
		if (compressed
			&& (parsed->m_width % block.blockWidth != 0 || parsed->m_height % block.blockHeight != 0))
		{
			result.Message = "size is not a whole number of blocks - stored uncompressed";
			dstFormat = bimg::TextureFormat::RGBA8;
			compressed = false;
		}

		// Everything below reads RGBA8, which the input already is: the mip filter, and the
		// per-band source offset arithmetic, which assumes four tightly packed bytes per pixel.
		bimg::ImageContainer* source = parsed;

		if (options.GenerateMips && source->m_numMips <= 1)
		{
			if (bimg::ImageContainer* mipped = bimg::imageGenerateMips(&s_Allocator, *source))
			{
				bimg::imageFree(source);
				source = mipped;
			}
		}

		// MaxSize starts the chain lower instead of resampling: the mip chain already holds
		// every halving of the source, so this is exact and free where a second filter would
		// be neither.
		uint8_t firstMip = 0;
		if (options.MaxSize > 0)
		{
			while (firstMip + 1 < source->m_numMips
				&& ((source->m_width >> firstMip) > options.MaxSize
					|| (source->m_height >> firstMip) > options.MaxSize))
			{
				firstMip++;
			}
		}

		const uint32_t outWidth = std::max(1u, source->m_width >> firstMip);
		const uint32_t outHeight = std::max(1u, source->m_height >> firstMip);
		const uint8_t mipCount = (uint8_t)(source->m_numMips - firstMip);

		bimg::ImageContainer* encoded = bimg::imageAlloc(&s_Allocator, dstFormat,
			(uint16_t)outWidth, (uint16_t)outHeight, 1, 1, false, mipCount > 1);

		if (!encoded)
		{
			bimg::imageFree(source);
			result.Message = "the compiled image could not be allocated";
			return false;
		}

		const bimg::Quality::Enum quality = (compressed && options.NormalMap)
			? bimg::Quality::NormalMapDefault : bimg::Quality::Default;

		const uint8_t writeMips = std::min<uint8_t>(mipCount, encoded->m_numMips);

		for (uint8_t lod = 0; lod < writeMips; lod++)
		{
			if (options.ShouldCancel && options.ShouldCancel())
			{
				bimg::imageFree(encoded);
				bimg::imageFree(source);
				result.Message = "cancelled";
				return false;
			}

			bimg::ImageMip srcMip;
			if (!bimg::imageGetRawData(*source, 0, (uint8_t)(firstMip + lod),
				source->m_data, source->m_size, srcMip))
			{
				continue;
			}

			bimg::ImageMip dstMip;
			if (!bimg::imageGetRawData(*encoded, 0, lod, encoded->m_data, encoded->m_size, dstMip))
				continue;

			uint8_t* dstData = const_cast<uint8_t*>(dstMip.m_data);

			if (compressed)
				EncodeMip(srcMip, dstData, dstMip.m_size, dstFormat, quality, options);
			else
				bx::memCopy(dstData, srcMip.m_data, std::min(dstMip.m_size, srcMip.m_size));
		}

		// The chain can be shorter than what imageAlloc laid out, when MaxSize cut the top off.
		// Mips are stored largest first, so a smaller count and a smaller size is the whole of
		// the truncation - nothing has to be repacked.
		const uint32_t writeSize = MipChainSize(*encoded, writeMips);
		encoded->m_numMips = writeMips;

		bx::MemoryBlock memory(&s_Allocator);
		bx::MemoryWriter writer(&memory);

		bx::Error writeErr;
		const int32_t written = bimg::imageWriteDds(&writer, *encoded, encoded->m_data, writeSize, &writeErr);
		const bool ok = written > 0 && writeErr.isOk();

		if (ok)
		{
			const uint8_t* bytes = (const uint8_t*)memory.more(0);
			result.Dds.assign(bytes, bytes + memory.getSize());
			result.Width = outWidth;
			result.Height = outHeight;
			result.MipCount = writeMips;
			result.Compressed = compressed;
		}
		else
		{
			result.Message = "the DDS container could not be written";
		}

		bimg::imageFree(encoded);
		bimg::imageFree(source);
		return ok;
	}

}
