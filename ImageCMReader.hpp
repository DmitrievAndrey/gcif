#ifndef IMAGE_CM_READER_HPP
#define IMAGE_CM_READER_HPP

#include "Platform.hpp"
#include "ImageReader.hpp"
#include "HuffmanDecoder.hpp"
#include "ImageMaskReader.hpp"
#include "ImageLZReader.hpp"
#include "GCIFReader.hpp"
#include "Filters.hpp"
#include "EntropyDecoder.hpp"

/*
 * Game Closure Context Modeling (GC-CM) Decompression
 *
 * The decompressor rebuilds the static Huffman tables generated by the encoder
 * and then iterates over each pixel from upper left to lower right.  Where the
 * Fully-Transparent Alpha mask is set, it emits a transparent black pixel.
 * Where the 2D LZ Exact Match algorithm triggers, it performs LZ decoding.
 *
 * For the remaining pixels, the BCIF "chaos" metric selects which Huffman
 * tables to use, and filtered pixel values are emitted.  The YUV color data is
 * then reversed to RGB and then the spatial filter is reversed back to the
 * original RGB data.
 *
 * LZ and alpha masking are very cheap decoding operations.  The most expensive
 * per-pixel operation is the static Huffman decoding, which is just a table
 * lookup and some bit twiddling for the majority of decoding.  As a result the
 * decoder is exceptionally fast.  It reaches for the Pareto Frontier.
 */

namespace cat {


//// ImageCMReader

class ImageCMReader {
public:
#ifdef FUZZY_CHAOS
	static const int CHAOS_LEVELS = 16;
#else
	static const int CHAOS_LEVELS = 8;
#endif

protected:
	u8 *_rgba;

	int _width, _height;
	u8 *_chaos;

	struct FilterSelection {
		SpatialFilterFunction sf;
		YUV2RGBFilterFunction cf;
	} *_filters; // hi4bits: sf, lo4bits: cf

	ImageMaskReader *_mask;
	ImageLZReader *_lz;

	HuffmanDecoder _sf, _cf;
	EntropyDecoder _decoder[3][CHAOS_LEVELS];

	void clear();

	int init(GCIFImage *image);
	int readFilterTables(ImageReader &reader);
	int readChaosTables(ImageReader &reader);
	int readRGB(ImageReader &reader);

#ifdef CAT_COLLECT_STATS
public:
	struct _Stats {
		double initUsec, readFilterTablesUsec, readChaosTablesUsec;
		double readRGBUsec, overallUsec;
	} Stats;
#endif

public:
	CAT_INLINE ImageCMReader() {
		_rgba = 0;
		_chaos = 0;
		_filters = 0;
	}
	virtual CAT_INLINE ~ImageCMReader() {
		clear();
	}

	int read(ImageReader &reader, ImageMaskReader &maskReader, ImageLZReader &lzReader, GCIFImage *image);

#ifdef CAT_COLLECT_STATS
	bool dumpStats();
#else
	CAT_INLINE bool dumpStats() {
		return false;
	}
#endif
};


} // namespace cat

#endif // IMAGE_CM_READER_HPP

