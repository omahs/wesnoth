/*
	Copyright (C) 2003 - 2024
	by David White <dave@whitevine.net>
	Part of the Battle for Wesnoth Project https://www.wesnoth.org/

	This program is free software; you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation; either version 2 of the License, or
	(at your option) any later version.
	This program is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY.

	See the COPYING file for more details.
*/

/**
 *  @file
 *  Support-routines for the SDL-graphics-library.
 */

#include "sdl/utils.hpp"
#include "sdl/rect.hpp"
#include "color.hpp"
#include "log.hpp"
#include "xBRZ/xbrz.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>

#include <boost/circular_buffer.hpp>
#include <boost/math/constants/constants.hpp>

static lg::log_domain log_display("display");
#define ERR_DP LOG_STREAM(err, log_display)

version_info sdl::get_version()
{
	SDL_version sdl_version;
	SDL_GetVersion(&sdl_version);
	return version_info(sdl_version.major, sdl_version.minor, sdl_version.patch);
}

bool sdl::runtime_at_least(uint8_t major, uint8_t minor, uint8_t patch)
{
	SDL_version ver;
	SDL_GetVersion(&ver);
	if(ver.major < major) return false;
	if(ver.major > major) return true;
	// major version equal
	if(ver.minor < minor) return false;
	if(ver.minor > minor) return true;
	// major and minor version equal
	if(ver.patch < patch) return false;
	return true;
}

surface scale_surface_xbrz(const surface & surf, std::size_t z)
{
	if(surf == nullptr)
		return nullptr;

	if (z > 5) {
		PLAIN_LOG << "Cannot use xbrz scaling with zoom factor > 5.";
		z = 1;
	}

	if (z == 1) {
		surface temp = surf; // TODO: no temp surface
		return temp;
	}

	surface dst(surf->w *z, surf->h * z);

	if (z == 0) {
		PLAIN_LOG << "Create an empty image";
		return dst;
	}

	if(surf == nullptr || dst == nullptr) {
		PLAIN_LOG << "Could not create surface to scale onto";
		return nullptr;
	}

	{
		const_surface_lock src_lock(surf);
		surface_lock dst_lock(dst);

		xbrz::scale(z, src_lock.pixels(), dst_lock.pixels(), surf->w, surf->h);
	}

	return dst;
}

surface scale_surface_nn (const surface & surf, int w, int h)
{
	// Since SDL version 1.1.5 0 is transparent, before 255 was transparent.
	assert(SDL_ALPHA_TRANSPARENT==0);

	if (surf == nullptr)
		return nullptr;

	if(w == surf->w && h == surf->h) {
		return surf;
	}
	assert(w >= 0);
	assert(h >= 0);

	surface dst(w,h);

	if (w == 0 || h ==0) {
		PLAIN_LOG << "Create an empty image";
		return dst;
	}

	if(surf == nullptr || dst == nullptr) {
		PLAIN_LOG << "Could not create surface to scale onto";
		return nullptr;
	}

	{
		const_surface_lock src_lock(surf);
		surface_lock dst_lock(dst);

		xbrz::nearestNeighborScale(src_lock.pixels(), surf->w, surf->h, dst_lock.pixels(), w, h);
	}

	return dst;
}

// NOTE: Don't pass this function 0 scaling arguments.
surface scale_surface(const surface &surf, int w, int h)
{
	// Since SDL version 1.1.5 0 is transparent, before 255 was transparent.
	assert(SDL_ALPHA_TRANSPARENT==0);

	if(surf == nullptr)
		return nullptr;

	if(w == surf->w && h == surf->h) {
		return surf;
	}
	assert(w >= 0);
	assert(h >= 0);

	surface dst(w,h);

	if (w == 0 || h ==0) {
		PLAIN_LOG << "Create an empty image";
		return dst;
	}

	if(surf == nullptr || dst == nullptr) {
		PLAIN_LOG << "Could not create surface to scale onto";
		return nullptr;
	}

	{
		const_surface_lock src_lock(surf);
		surface_lock dst_lock(dst);

		const uint32_t* const src_pixels = src_lock.pixels();
		uint32_t* const dst_pixels = dst_lock.pixels();

		int32_t xratio = fixed_point_divide(surf->w,w);
		int32_t yratio = fixed_point_divide(surf->h,h);

		int32_t ysrc = 0;
		for(int ydst = 0; ydst != h; ++ydst, ysrc += yratio) {
			int32_t xsrc = 0;
			for(int xdst = 0; xdst != w; ++xdst, xsrc += xratio) {
				const int xsrcint = fixed_point_to_int(xsrc);
				const int ysrcint = fixed_point_to_int(ysrc);

				const uint32_t* const src_word = src_pixels + ysrcint*surf->w + xsrcint;
				uint32_t* const dst_word = dst_pixels +    ydst*dst->w + xdst;
				const int dx = (xsrcint + 1 < surf->w) ? 1 : 0;
				const int dy = (ysrcint + 1 < surf->h) ? surf->w : 0;

				uint8_t r,g,b,a;
				uint32_t rr,gg,bb,aa, temp;

				uint32_t pix[4], bilin[4];

				// This next part is the fixed point
				// equivalent of "take everything to
				// the right of the decimal point."
				// These fundamental weights decide
				// the contributions from various
				// input pixels. The labels assume
				// that the upper left corner of the
				// screen ("northeast") is 0,0 but the
				// code should still be consistent if
				// the graphics origin is actually
				// somewhere else.
				//
				// That is, the bilin array holds the
				// "geometric" weights. I.E. If I'm scaling
				// a 2 x 2 block a 10 x 10 block, then for
				// pixel (2,2) of output, the upper left
				// pixel should be 10:1 more influential than
				// the upper right, and also 10:1 more influential
				// than lower left, and 100:1 more influential
				// than lower right.

				const int32_t e = 0x000000FF & xsrc;
				const int32_t s = 0x000000FF & ysrc;
				const int32_t n = 0xFF - s;
				// Not called "w" to avoid hiding a function parameter
				// (would cause a compiler warning in MSVC2015 with /W4)
				const int32_t we = 0xFF - e;

				pix[0] = *src_word;              // northwest
				pix[1] = *(src_word + dx);       // northeast
				pix[2] = *(src_word + dy);       // southwest
				pix[3] = *(src_word + dx + dy);  // southeast

				bilin[0] = n*we;
				bilin[1] = n*e;
				bilin[2] = s*we;
				bilin[3] = s*e;

				int loc;
				rr = bb = gg = aa = 0;
				for (loc=0; loc<4; loc++) {
				  a = pix[loc] >> 24;
				  r = pix[loc] >> 16;
				  g = pix[loc] >> 8;
				  b = pix[loc] >> 0;

				  //We also have to implement weighting by alpha for the RGB components
				  //If a unit has some parts solid and some parts translucent,
				  //i.e. a red cloak but a dark shadow, then when we scale in
				  //the shadow shouldn't appear to become red at the edges.
				  //This part also smoothly interpolates between alpha=0 being
				  //transparent and having no contribution, vs being opaque.
				  temp = (a * bilin[loc]);
				  rr += r * temp;
				  gg += g * temp;
				  bb += b * temp;
				  aa += temp;
				}

				a = aa >> (16); // we average the alphas, they don't get weighted by any other factor besides bilin
				if (a != 0) {
					rr /= a;	// finish alpha weighting: divide by sum of alphas
					gg /= a;
					bb /= a;
				}
				r = rr >> (16); // now shift over by 16 for the bilin part
				g = gg >> (16);
				b = bb >> (16);
				*dst_word = (a << 24) + (r << 16) + (g << 8) + b;
			}
		}
	}

	return dst;
}

surface scale_surface_legacy(const surface &surf, int w, int h)
{
	// Since SDL version 1.1.5 0 is transparent, before 255 was transparent.
	assert(SDL_ALPHA_TRANSPARENT==0);

	if(surf == nullptr)
		return nullptr;

	if(w == surf->w && h == surf->h) {
		return surf;
	}
	assert(w >= 0);
	assert(h >= 0);

	surface dst(w,h);

	if(surf == nullptr || dst == nullptr) {
		PLAIN_LOG << "Could not create surface to scale onto";
		return nullptr;
	}

	{
		const_surface_lock src_lock(surf);
		surface_lock dst_lock(dst);

		const uint32_t* const src_pixels = src_lock.pixels();
		uint32_t* const dst_pixels = dst_lock.pixels();

		int32_t xratio = fixed_point_divide(surf->w,w);
		int32_t yratio = fixed_point_divide(surf->h,h);

		int32_t ysrc = 0;
		for(int ydst = 0; ydst != h; ++ydst, ysrc += yratio) {
			int32_t xsrc = 0;
			for(int xdst = 0; xdst != w; ++xdst, xsrc += xratio) {
				const int xsrcint = fixed_point_to_int(xsrc);
				const int ysrcint = fixed_point_to_int(ysrc);

				const uint32_t* const src_word = src_pixels + ysrcint*surf->w + xsrcint;
				uint32_t* const dst_word = dst_pixels +    ydst*dst->w + xdst;
				const int dx = (xsrcint + 1 < surf->w) ? 1 : 0;
				const int dy = (ysrcint + 1 < surf->h) ? surf->w : 0;

				uint8_t r,g,b,a;
				uint32_t rr,gg,bb,aa;
				uint16_t avg_r, avg_g, avg_b;
				uint32_t pix[4], bilin[4];

				// This next part is the fixed point
				// equivalent of "take everything to
				// the right of the decimal point."
				// These fundamental weights decide
				// the contributions from various
				// input pixels. The labels assume
				// that the upper left corner of the
				// screen ("northeast") is 0,0 but the
				// code should still be consistent if
				// the graphics origin is actually
				// somewhere else.

				const int32_t east = 0x000000FF & xsrc;
				const int32_t south = 0x000000FF & ysrc;
				const int32_t north = 0xFF - south;
				const int32_t west = 0xFF - east;

				pix[0] = *src_word;              // northwest
				pix[1] = *(src_word + dx);       // northeast
				pix[2] = *(src_word + dy);       // southwest
				pix[3] = *(src_word + dx + dy);  // southeast

				bilin[0] = north*west;
				bilin[1] = north*east;
				bilin[2] = south*west;
				bilin[3] = south*east;

				// Scope out the neighboorhood, see
				// what the pixel values are like.

				int count = 0;
				avg_r = avg_g = avg_b = 0;
				int loc;
				for (loc=0; loc<4; loc++) {
				  a = pix[loc] >> 24;
				  r = pix[loc] >> 16;
				  g = pix[loc] >> 8;
				  b = pix[loc] >> 0;
				  if (a != 0) {
				    avg_r += r;
				    avg_g += g;
				    avg_b += b;
				    count++;
				  }
				}
				if (count>0) {
				  avg_r /= count;
				  avg_b /= count;
				  avg_g /= count;
				}

				// Perform modified bilinear interpolation.
				// Don't trust any color information from
				// an RGBA sample when the alpha channel
				// is set to fully transparent.
				//
				// Some of the input images are hex tiles,
				// created using a hexagon shaped alpha channel
				// that is either set to full-on or full-off.

				rr = gg = bb = aa = 0;
				for (loc=0; loc<4; loc++) {
				  a = pix[loc] >> 24;
				  r = pix[loc] >> 16;
				  g = pix[loc] >> 8;
				  b = pix[loc] >> 0;
				  if (a == 0) {
				    r = static_cast<uint8_t>(avg_r);
				    g = static_cast<uint8_t>(avg_g);
				    b = static_cast<uint8_t>(avg_b);
				  }
				  rr += r * bilin[loc];
				  gg += g * bilin[loc];
				  bb += b * bilin[loc];
				  aa += a * bilin[loc];
				}
				r = rr >> 16;
				g = gg >> 16;
				b = bb >> 16;
				a = aa >> 16;
				*dst_word = (a << 24) + (r << 16) + (g << 8) + b;
			}
		}
	}

	return dst;
}


surface scale_surface_sharp(const surface& surf, int w, int h)
{
	// Since SDL version 1.1.5 0 is transparent, before 255 was transparent.
	assert(SDL_ALPHA_TRANSPARENT == 0);

	if(surf == nullptr) {
		return nullptr;
	}
	if(w == surf->w && h == surf->h) {
		return surf;
	}

	assert(w >= 0);
	assert(h >= 0);
	surface dst(w, h);
	if(dst == nullptr) {
		PLAIN_LOG << "Could not create surface to scale onto";
		return nullptr;
	}

	if(w == 0 || h == 0) {
		PLAIN_LOG << "Creating an empty image";
		return dst;
	}

	{
		const_surface_lock src_lock(surf);
		surface_lock dst_lock(dst);

		const uint32_t* const src_pixels = src_lock.pixels();
		uint32_t* const dst_pixels = dst_lock.pixels();

		const int src_w = surf->w;
		const int src_h = surf->h;

		const float xratio = static_cast<float>(src_w) / static_cast<float>(w);
		const float yratio = static_cast<float>(src_h) / static_cast<float>(h);
		for(int ydst = 0; ydst != h; ++ydst) {
			for(int xdst = 0; xdst != w; ++xdst) {
				// Project dst pixel to a single corresponding src pixel by scale and simply take it
				const int xsrc = std::floor(static_cast<float>(xdst) * xratio);
				const int ysrc = std::floor(static_cast<float>(ydst) * yratio);
				dst_pixels[ydst * dst->w + xdst] = src_pixels[ysrc * src_w + xsrc];
			}
		}
	}

	return dst;
}

surface adjust_surface_color(const surface &surf, int red, int green, int blue)
{
	if(surf == nullptr)
		return nullptr;

	if((red == 0 && green == 0 && blue == 0)) {
		surface temp = surf; // TODO: remove temp surface
		return temp;
	}

	surface nsurf = surf.clone();

	if(nsurf == nullptr) {
		PLAIN_LOG << "failed to make neutral surface";
		return nullptr;
	}

	{
		surface_lock lock(nsurf);
		uint32_t* beg = lock.pixels();
		uint32_t* end = beg + nsurf->w*surf->h;

		while(beg != end) {
			uint8_t alpha = (*beg) >> 24;

			if(alpha) {
				uint8_t r, g, b;
				r = (*beg) >> 16;
				g = (*beg) >> 8;
				b = (*beg) >> 0;

				r = std::max<int>(0,std::min<int>(255,static_cast<int>(r)+red));
				g = std::max<int>(0,std::min<int>(255,static_cast<int>(g)+green));
				b = std::max<int>(0,std::min<int>(255,static_cast<int>(b)+blue));

				*beg = (alpha << 24) + (r << 16) + (g << 8) + b;
			}

			++beg;
		}
	}

	return nsurf;
}

surface greyscale_image(const surface &surf)
{
	if(surf == nullptr)
		return nullptr;

	surface nsurf = surf.clone();
	if(nsurf == nullptr) {
		PLAIN_LOG << "failed to make neutral surface";
		return nullptr;
	}

	{
		surface_lock lock(nsurf);
		uint32_t* beg = lock.pixels();
		uint32_t* end = beg + nsurf->w*surf->h;

		while(beg != end) {
			uint8_t alpha = (*beg) >> 24;

			if(alpha) {
				uint8_t r, g, b;
				r = (*beg) >> 16;
				g = (*beg) >> 8;
				b = (*beg);
				//const uint8_t avg = (red+green+blue)/3;

				// Use the correct formula for RGB to grayscale conversion.
				// Ok, this is no big deal :)
				// The correct formula being:
				// gray=0.299red+0.587green+0.114blue
				const uint8_t avg = static_cast<uint8_t>((
					77  * static_cast<uint16_t>(r) +
					150 * static_cast<uint16_t>(g) +
					29  * static_cast<uint16_t>(b)  ) / 256);

				*beg = (alpha << 24) | (avg << 16) | (avg << 8) | avg;
			}

			++beg;
		}
	}

	return nsurf;
}

surface monochrome_image(const surface &surf, const int threshold)
{
	if(surf == nullptr)
		return nullptr;

	surface nsurf = surf.clone();
	if(nsurf == nullptr) {
		PLAIN_LOG << "failed to make neutral surface";
		return nullptr;
	}

	{
		surface_lock lock(nsurf);
		uint32_t* beg = lock.pixels();
		uint32_t* end = beg + nsurf->w*surf->h;

		while(beg != end) {
			uint8_t alpha = (*beg) >> 24;

			if(alpha) {
				uint8_t r, g, b, result;
				r = (*beg) >> 16;
				g = (*beg) >> 8;
				b = (*beg);

				// first convert the pixel to grayscale
				// if the resulting value is above the threshold make it black
				// else make it white
				result = static_cast<uint8_t>(0.299 * r + 0.587 * g + 0.114 * b) > threshold ? 255 : 0;

				*beg = (alpha << 24) | (result << 16) | (result << 8) | result;
			}

			++beg;
		}
	}

	return nsurf;
}

surface sepia_image(const surface &surf)
{
	if(surf == nullptr)
		return nullptr;

	surface nsurf = surf.clone();
	if(nsurf == nullptr) {
		PLAIN_LOG << "failed to make neutral surface";
		return nullptr;
	}

	{
		surface_lock lock(nsurf);
		uint32_t* beg = lock.pixels();
		uint32_t* end = beg + nsurf->w*surf->h;

		while(beg != end) {
			uint8_t alpha = (*beg) >> 24;

			if(alpha) {
				uint8_t r, g, b;
				r = (*beg) >> 16;
				g = (*beg) >> 8;
				b = (*beg);

				// this is the formula for applying a sepia effect
				// that can be found on various web sites
				// for example here: https://software.intel.com/sites/default/files/article/346220/sepiafilter-intelcilkplus.pdf
				uint8_t outRed = std::min(255, static_cast<int>((r * 0.393) + (g * 0.769) + (b * 0.189)));
				uint8_t outGreen = std::min(255, static_cast<int>((r * 0.349) + (g * 0.686) + (b * 0.168)));
				uint8_t outBlue = std::min(255, static_cast<int>((r * 0.272) + (g * 0.534) + (b * 0.131)));

				*beg = (alpha << 24) | (outRed << 16) | (outGreen << 8) | (outBlue);
			}

			++beg;
		}
	}

	return nsurf;
}

surface negative_image(const surface &surf, const int thresholdR, const int thresholdG, const int thresholdB)
{
	if(surf == nullptr)
		return nullptr;

	surface nsurf = surf.clone();
	if(nsurf == nullptr) {
		PLAIN_LOG << "failed to make neutral surface";
		return nullptr;
	}

	{
		surface_lock lock(nsurf);
		uint32_t* beg = lock.pixels();
		uint32_t* end = beg + nsurf->w*surf->h;

		while(beg != end) {
			uint8_t alpha = (*beg) >> 24;

			if(alpha) {
				uint8_t r, g, b, newR, newG, newB;
				r = (*beg) >> 16;
				g = (*beg) >> 8;
				b = (*beg);

				// invert he channel only if its value is greater than the supplied threshold
				// this can be used for solarization effects
				// for a full negative effect, use a value of -1
				// 255 is a no-op value (doesn't do anything, since a uint8_t cannot contain a greater value than that)
				newR = r > thresholdR ? 255 - r : r;
				newG = g > thresholdG ? 255 - g : g;
				newB = b > thresholdB ? 255 - b : b;

				*beg = (alpha << 24) | (newR << 16) | (newG << 8) | (newB);
			}

			++beg;
		}
	}

	return nsurf;
}

surface alpha_to_greyscale(const surface &surf)
{
	if(surf == nullptr)
		return nullptr;

	surface nsurf = surf.clone();
	if(nsurf == nullptr) {
		PLAIN_LOG << "failed to make neutral surface";
		return nullptr;
	}

	{
		surface_lock lock(nsurf);
		uint32_t* beg = lock.pixels();
		uint32_t* end = beg + nsurf->w*surf->h;

		while(beg != end) {
			uint8_t alpha = (*beg) >> 24;

			*beg = (0xff << 24) | (alpha << 16) | (alpha << 8) | alpha;

			++beg;
		}
	}

	return nsurf;
}

surface wipe_alpha(const surface &surf)
{
	if(surf == nullptr)
		return nullptr;

	surface nsurf = surf.clone();
	if(nsurf == nullptr) {
		PLAIN_LOG << "failed to make neutral surface";
		return nullptr;
	}

	{
		surface_lock lock(nsurf);
		uint32_t* beg = lock.pixels();
		uint32_t* end = beg + nsurf->w*surf->h;

		while(beg != end) {

			*beg = 0xff000000 | *beg;

			++beg;
		}
	}

	return nsurf;
}


surface shadow_image(const surface &surf, int scale)
{
	if(surf == nullptr)
		return nullptr;

	// we blur it, and reuse the neutral surface created by the blur function
	surface nsurf (blur_alpha_surface(surf, 2*scale));

	if(nsurf == nullptr) {
		PLAIN_LOG << "failed to blur the shadow surface";
		return nullptr;
	}

	{
		surface_lock lock(nsurf);
		uint32_t* beg = lock.pixels();
		uint32_t* end = beg + nsurf->w*surf->h;

		while(beg != end) {
			uint8_t alpha = (*beg) >> 24;

			if(alpha) {
				// increase alpha and color in black (RGB=0)
				// with some stupid optimization for handling maximum values
				if (alpha < 255/4)
					*beg = (alpha*4) << 24;
				else
					*beg = 0xFF000000; // we hit the maximum
			}

			++beg;
		}
	}

	return nsurf;
}

surface swap_channels_image(const surface& surf, channel r, channel g, channel b, channel a) {
	if(surf == nullptr)
		return nullptr;

	surface nsurf = surf.clone();
	if(nsurf == nullptr) {
		PLAIN_LOG << "failed to make neutral surface";
		return nullptr;
	}

	{
		surface_lock lock(nsurf);
		uint32_t* beg = lock.pixels();
		uint32_t* end = beg + nsurf->w*surf->h;

		while(beg != end) {
			uint8_t alpha = (*beg) >> 24;

			if(alpha) {
				uint8_t red, green, blue, newRed, newGreen, newBlue, newAlpha;
				red = (*beg) >> 16;
				green = (*beg) >> 8;
				blue = (*beg);

				switch (r) {
					case RED:
						newRed = red;
						break;
					case GREEN:
						newRed = green;
						break;
					case BLUE:
						newRed = blue;
						break;
					case ALPHA:
						newRed = alpha;
						break;
					default:
						return nullptr;
				}

				switch (g) {
					case RED:
						newGreen = red;
						break;
					case GREEN:
						newGreen = green;
						break;
					case BLUE:
						newGreen = blue;
						break;
					case ALPHA:
						newGreen = alpha;
						break;
					default:
						return nullptr;
				}

				switch (b) {
					case RED:
						newBlue = red;
						break;
					case GREEN:
						newBlue = green;
						break;
					case BLUE:
						newBlue = blue;
						break;
					case ALPHA:
						newBlue = alpha;
						break;
					default:
						return nullptr;
				}

				switch (a) {
					case RED:
						newAlpha = red;
						break;
					case GREEN:
						newAlpha = green;
						break;
					case BLUE:
						newAlpha = blue;
						break;
					case ALPHA:
						newAlpha = alpha;
						break;
					default:
						return nullptr;
				}

				*beg = (newAlpha << 24) | (newRed << 16) | (newGreen << 8) | newBlue;
			}

			++beg;
		}
	}

	return nsurf;
}

surface recolor_image(surface surf, const color_range_map& map_rgb)
{
	if(surf == nullptr)
		return nullptr;

	if(map_rgb.empty()) {
		return surf;
	}

	surface nsurf = surf.clone();
	if(nsurf == nullptr) {
		PLAIN_LOG << "failed to make neutral surface";
		return nullptr;
	}

	surface_lock lock(nsurf);
	uint32_t* beg = lock.pixels();
	uint32_t* end = beg + nsurf->w*surf->h;

	while(beg != end) {
		uint8_t alpha = (*beg) >> 24;

		// Don't recolor invisible pixels.
		if(alpha) {
			// Palette use only RGB channels, so remove alpha
			uint32_t oldrgb = (*beg) | 0xFF000000;

			auto i = map_rgb.find(color_t::from_argb_bytes(oldrgb));
			if(i != map_rgb.end()) {
				*beg = (alpha << 24) | (i->second.to_argb_bytes() & 0x00FFFFFF);
			}
		}

		++beg;
	}

	return nsurf;
}

surface brighten_image(const surface &surf, int32_t amount)
{
	if(surf == nullptr) {
		return nullptr;
	}

	surface nsurf = surf.clone();

	if(nsurf == nullptr) {
		PLAIN_LOG << "could not make neutral surface...";
		return nullptr;
	}

	{
		surface_lock lock(nsurf);
		uint32_t* beg = lock.pixels();
		uint32_t* end = beg + nsurf->w*surf->h;

		if (amount < 0) amount = 0;
		while(beg != end) {
			uint8_t alpha = (*beg) >> 24;

			if(alpha) {
				uint8_t r, g, b;
				r = (*beg) >> 16;
				g = (*beg) >> 8;
				b = (*beg);

				r = std::min<unsigned>(fixed_point_multiply(r, amount),255);
				g = std::min<unsigned>(fixed_point_multiply(g, amount),255);
				b = std::min<unsigned>(fixed_point_multiply(b, amount),255);

				*beg = (alpha << 24) + (r << 16) + (g << 8) + b;
			}

			++beg;
		}
	}

	return nsurf;
}

void adjust_surface_alpha(surface& surf, uint8_t alpha_mod)
{
	if(surf == nullptr) {
		return;
	}

	SDL_SetSurfaceAlphaMod(surf, alpha_mod);
}

surface adjust_surface_alpha_add(const surface &surf, int amount)
{
	if(surf== nullptr) {
		return nullptr;
	}

	surface nsurf = surf.clone();

	if(nsurf == nullptr) {
		PLAIN_LOG << "could not make neutral surface...";
		return nullptr;
	}

	{
		surface_lock lock(nsurf);
		uint32_t* beg = lock.pixels();
		uint32_t* end = beg + nsurf->w*surf->h;

		while(beg != end) {
			uint8_t alpha = (*beg) >> 24;

			if(alpha) {
				uint8_t r, g, b;
				r = (*beg) >> 16;
				g = (*beg) >> 8;
				b = (*beg);

				alpha = uint8_t(std::max<int>(0,std::min<int>(255,static_cast<int>(alpha) + amount)));
				*beg = (alpha << 24) + (r << 16) + (g << 8) + b;
			}

			++beg;
		}
	}

	return nsurf;
}

surface mask_surface(const surface &surf, const surface &mask, bool* empty_result, const std::string& filename)
{
	if(surf == nullptr) {
		*empty_result = true;
		return nullptr;
	}
	if(mask == nullptr) {
		return surf;
	}

	surface nsurf = surf.clone();
	surface nmask = mask.clone();

	if(nsurf == nullptr || nmask == nullptr) {
		PLAIN_LOG << "could not make neutral surface...";
		return nullptr;
	}
	if (nsurf->w !=  nmask->w) {
		// we don't support efficiently different width.
		// (different height is not a real problem)
		// This function is used on all hexes and usually only for that
		// so better keep it simple and efficient for the normal case
		std::stringstream ss;
		ss << "Detected an image with bad dimensions: ";
		if(!filename.empty()) ss << filename << ": ";
		ss << nsurf->w << "x" << nsurf->h;
		PLAIN_LOG << ss.str();
		PLAIN_LOG << "It will not be masked, please use: "<< nmask->w << "x" << nmask->h;
		return nsurf;
	}

	bool empty = true;
	{
		surface_lock lock(nsurf);
		const_surface_lock mlock(nmask);

		uint32_t* beg = lock.pixels();
		uint32_t* end = beg + nsurf->w*surf->h;
		const uint32_t* mbeg = mlock.pixels();
		const uint32_t* mend = mbeg + nmask->w*nmask->h;

		while(beg != end && mbeg != mend) {
			uint8_t alpha = (*beg) >> 24;

			if(alpha) {
				uint8_t r, g, b;
				r = (*beg) >> 16;
				g = (*beg) >> 8;
				b = (*beg);

				uint8_t malpha = (*mbeg) >> 24;
				if (alpha > malpha) {
					alpha = malpha;
				}
				if(alpha)
					empty = false;

				*beg = (alpha << 24) + (r << 16) + (g << 8) + b;
			}

			++beg;
			++mbeg;
		}
	}
	if(empty_result)
		*empty_result = empty;

	return nsurf;
}

bool in_mask_surface(const surface &surf, const surface &mask)
{
	if(surf == nullptr) {
		return false;
	}
	if(mask == nullptr){
		return true;
	}

	if (surf->w != mask->w || surf->h != mask->h ) {
		// not same size, consider it doesn't fit
		return false;
	}

	surface nsurf = surf.clone();
	surface nmask = mask.clone();

	if(nsurf == nullptr || nmask == nullptr) {
		PLAIN_LOG << "could not make neutral surface...";
		return false;
	}

	{
		surface_lock lock(nsurf);
		const_surface_lock mlock(nmask);

		const uint32_t* mbeg = mlock.pixels();
		const uint32_t* mend = mbeg + nmask->w*nmask->h;
		uint32_t* beg = lock.pixels();
		// no need for 'end', because both surfaces have same size

		while(mbeg != mend) {
			uint8_t malpha = (*mbeg) >> 24;
			if(malpha == 0) {
				uint8_t alpha = (*beg) >> 24;
				if (alpha)
					return false;
			}
			++mbeg;
			++beg;
		}
	}

	return true;
}

surface light_surface(const surface &surf, const surface &lightmap)
{
	if(surf == nullptr) {
		return nullptr;
	}
	if(lightmap == nullptr) {
		return surf;
	}

	surface nsurf = surf.clone();

	if(nsurf == nullptr) {
		PLAIN_LOG << "could not make neutral surface...";
		return nullptr;
	}
	if (nsurf->w != lightmap->w) {
		// we don't support efficiently different width.
		// (different height is not a real problem)
		// This function is used on all hexes and usually only for that
		// so better keep it simple and efficient for the normal case
		PLAIN_LOG << "Detected an image with bad dimensions: " << nsurf->w << "x" << nsurf->h;
		PLAIN_LOG << "It will not be lighted, please use: "<< lightmap->w << "x" << lightmap->h;
		return nsurf;
	}
	{
		surface_lock lock(nsurf);
		const_surface_lock llock(lightmap);

		uint32_t* beg = lock.pixels();
		uint32_t* end = beg + nsurf->w * nsurf->h;
		const uint32_t* lbeg = llock.pixels();
		const uint32_t* lend = lbeg + lightmap->w * lightmap->h;

		while(beg != end && lbeg != lend) {
			uint8_t alpha = (*beg) >> 24;
			if(alpha) {
				uint8_t lr, lg, lb;

				lr = (*lbeg) >> 16;
				lg = (*lbeg) >> 8;
				lb = (*lbeg);

				uint8_t r, g, b;
				r = (*beg) >> 16;
				g = (*beg) >> 8;
				b = (*beg);

				int dr = (static_cast<int>(lr) - 128) * 2;
				int dg = (static_cast<int>(lg) - 128) * 2;
				int db = (static_cast<int>(lb) - 128) * 2;
				//note that r + dr will promote r to int (needed to avoid uint8_t math)
				r = std::max<int>(0,std::min<int>(255, r + dr));
				g = std::max<int>(0,std::min<int>(255, g + dg));
				b = std::max<int>(0,std::min<int>(255, b + db));

				*beg = (alpha << 24) + (r << 16) + (g << 8) + b;
			}
			++beg;
			++lbeg;
		}
	}

	return nsurf;
}


surface blur_surface(const surface &surf, int depth)
{
	if(surf == nullptr) {
		return nullptr;
	}

	surface res = surf.clone();

	if(res == nullptr) {
		PLAIN_LOG << "could not make neutral surface...";
		return nullptr;
	}

	SDL_Rect rect {0, 0, surf->w, surf->h};
	blur_surface(res, rect, depth);

	return res;
}

void blur_surface(surface& surf, SDL_Rect rect, int depth)
{
	if(surf == nullptr) {
		return;
	}

	const int max_blur = 256;
	if(depth > max_blur) {
		depth = max_blur;
	}

	uint32_t queue[max_blur];
	const uint32_t* end_queue = queue + max_blur;

	const uint32_t ff = 0xff;

	const unsigned pixel_offset = rect.y * surf->w + rect.x;

	surface_lock lock(surf);
	for(int y = 0; y < rect.h; ++y) {
		const uint32_t* front = &queue[0];
		uint32_t* back = &queue[0];
		uint32_t red = 0, green = 0, blue = 0, avg = 0;
		uint32_t* p = lock.pixels() + pixel_offset + y * surf->w;
		for(int x = 0; x <= depth && x < rect.w; ++x, ++p) {
			red += ((*p) >> 16)&0xFF;
			green += ((*p) >> 8)&0xFF;
			blue += (*p)&0xFF;
			++avg;
			*back++ = *p;
			if(back == end_queue) {
				back = &queue[0];
			}
		}

		p = lock.pixels() + pixel_offset + y * surf->w;
		for(int x = 0; x < rect.w; ++x, ++p) {
			*p = 0xFF000000
					| (std::min(red/avg,ff) << 16)
					| (std::min(green/avg,ff) << 8)
					| std::min(blue/avg,ff);

			if(x >= depth) {
				red -= ((*front) >> 16)&0xFF;
				green -= ((*front) >> 8)&0xFF;
				blue -= *front&0xFF;
				--avg;
				++front;
				if(front == end_queue) {
					front = &queue[0];
				}
			}

			if(x + depth+1 < rect.w) {
				uint32_t* q = p + depth+1;
				red += ((*q) >> 16)&0xFF;
				green += ((*q) >> 8)&0xFF;
				blue += (*q)&0xFF;
				++avg;
				*back++ = *q;
				if(back == end_queue) {
					back = &queue[0];
				}
			}
		}
	}

	for(int x = 0; x < rect.w; ++x) {
		const uint32_t* front = &queue[0];
		uint32_t* back = &queue[0];
		uint32_t red = 0, green = 0, blue = 0, avg = 0;
		uint32_t* p = lock.pixels() + pixel_offset + x;
		for(int y = 0; y <= depth && y < rect.h; ++y, p += surf->w) {
			red += ((*p) >> 16)&0xFF;
			green += ((*p) >> 8)&0xFF;
			blue += *p&0xFF;
			++avg;
			*back++ = *p;
			if(back == end_queue) {
				back = &queue[0];
			}
		}

		p = lock.pixels() + pixel_offset + x;
		for(int y = 0; y < rect.h; ++y, p += surf->w) {
			*p = 0xFF000000
					| (std::min(red/avg,ff) << 16)
					| (std::min(green/avg,ff) << 8)
					| std::min(blue/avg,ff);

			if(y >= depth) {
				red -= ((*front) >> 16)&0xFF;
				green -= ((*front) >> 8)&0xFF;
				blue -= *front&0xFF;
				--avg;
				++front;
				if(front == end_queue) {
					front = &queue[0];
				}
			}

			if(y + depth+1 < rect.h) {
				uint32_t* q = p + (depth+1)*surf->w;
				red += ((*q) >> 16)&0xFF;
				green += ((*q) >> 8)&0xFF;
				blue += (*q)&0xFF;
				++avg;
				*back++ = *q;
				if(back == end_queue) {
					back = &queue[0];
				}
			}
		}
	}
}

surface blur_alpha_surface(const surface &surf, int depth)
{
	if(surf == nullptr) {
		return nullptr;
	}

	surface res = surf.clone();

	if(res == nullptr) {
		PLAIN_LOG << "could not make neutral surface...";
		return nullptr;
	}

	const int max_blur = 256;
	if(depth > max_blur) {
		depth = max_blur;
	}

	struct Pixel{
		uint8_t alpha;
		uint8_t red;
		uint8_t green;
		uint8_t blue;
		Pixel(uint32_t* p)
		  : alpha(((*p) >> 24)&0xFF)
		  , red(((*p) >> 16)&0xFF)
		  , green(((*p) >> 8)&0xFF)
		  , blue((*p)&0xFF) {}
	};
	struct Average{
		uint32_t alpha;
		uint32_t red;
		uint32_t green;
		uint32_t blue;
		Average() : alpha(), red(), green(), blue()
		{}
		Average& operator+=(const Pixel& pix){
			red   += pix.alpha * pix.red;
			green += pix.alpha * pix.green;
			blue  += pix.alpha * pix.blue;
			alpha += pix.alpha;
			return *this;
		}
		Average& operator-=(const Pixel& pix){
			red   -= pix.alpha * pix.red;
			green -= pix.alpha * pix.green;
			blue  -= pix.alpha * pix.blue;
			alpha -= pix.alpha;
			return *this;
		}
		uint32_t operator()(unsigned num){
			const uint32_t ff = 0xff;
			if(!alpha){
				return 0;
			}
			return (std::min(alpha/num,ff) << 24)
			    | (std::min(red/alpha,ff) << 16)
			    | (std::min(green/alpha,ff) << 8)
			    | std::min(blue/alpha,ff);
		}
	};

	boost::circular_buffer<Pixel> queue(depth*2+1);

	surface_lock lock(res);
	int x, y;
	// Iterate over rows, blurring each row horizontally
	for(y = 0; y < res->h; ++y) {
		// Sum of pixel values stored here
		Average avg;

		// Preload the first depth+1 pixels
		uint32_t* p = lock.pixels() + y*res->w;
		for(x = 0; x <= depth && x < res->w; ++x, ++p) {
			assert(!queue.full());
			queue.push_back(Pixel{p});
			avg += queue.back();
		}

		// This is the actual inner loop
		p = lock.pixels() + y*res->w;
		for(x = 0; x < res->w; ++x, ++p) {
			// Write the current average
			const uint32_t num = queue.size();
			*p = avg(num);

			// Unload earlier pixels that are now too far away
			if(x >= depth) {
				avg -= queue.front();
				assert(!queue.empty());
				queue.pop_front();
			}

			// Add new pixels
			if(x + depth+1 < res->w) {
				uint32_t* q = p + depth+1;
				assert(!queue.full());
				queue.push_back(Pixel{q});
				avg += queue.back();
			}
		}
		assert(static_cast<int>(queue.size()) == std::min(depth, res->w));
		queue.clear();
	}

	// Iterate over columns, blurring each column vertically
	for(x = 0; x < res->w; ++x) {
		// Sum of pixel values stored here
		Average avg;

		// Preload the first depth+1 pixels
		uint32_t* p = lock.pixels() + x;
		for(y = 0; y <= depth && y < res->h; ++y, p += res->w) {
			assert(!queue.full());
			queue.push_back(Pixel{p});
			avg += queue.back();
		}

		// This is the actual inner loop
		p = lock.pixels() + x;
		for(y = 0; y < res->h; ++y, p += res->w) {
			// Write the current average
			const uint32_t num = queue.size();
			*p = avg(num);

			// Unload earlier pixels that are now too far away
			if(y >= depth) {
				avg -= queue.front();
				assert(!queue.empty());
				queue.pop_front();
			}

			// Add new pixels
			if(y + depth+1 < res->h) {
				uint32_t* q = p + (depth+1)*res->w;
				assert(!queue.full());
				queue.push_back(Pixel{q});
				avg += queue.back();
			}
		}
		assert(static_cast<int>(queue.size()) == std::min(depth, res->h));
		queue.clear();
	}

	return res;
}

surface cut_surface(const surface &surf, const SDL_Rect& r)
{
	if(surf == nullptr)
		return nullptr;

	surface res(r.w, r.h);

	if(res == nullptr) {
		PLAIN_LOG << "Could not create a new surface in cut_surface()";
		return nullptr;
	}

	std::size_t sbpp = surf->format->BytesPerPixel;
	std::size_t spitch = surf->pitch;
	std::size_t rbpp = res->format->BytesPerPixel;
	std::size_t rpitch = res->pitch;

	// compute the areas to copy
	SDL_Rect src_rect = r;
	SDL_Rect dst_rect { 0, 0, r.w, r.h };

	if (src_rect.x < 0) {
		if (src_rect.x + src_rect.w <= 0)
			return res;
		dst_rect.x -= src_rect.x;
		dst_rect.w += src_rect.x;
		src_rect.w += src_rect.x;
		src_rect.x = 0;
	}
	if (src_rect.y < 0) {
		if (src_rect.y + src_rect.h <= 0)
			return res;
		dst_rect.y -= src_rect.y;
		dst_rect.h += src_rect.y;
		src_rect.h += src_rect.y;
		src_rect.y = 0;
	}

	if(src_rect.x >= surf->w || src_rect.y >= surf->h)
		return res;

	const_surface_lock slock(surf);
	surface_lock rlock(res);

	const uint8_t* src = reinterpret_cast<const uint8_t *>(slock.pixels());
	uint8_t* dest = reinterpret_cast<uint8_t *>(rlock.pixels());

	for(int y = 0; y < src_rect.h && (src_rect.y + y) < surf->h; ++y) {
		const uint8_t* line_src  = src  + (src_rect.y + y) * spitch + src_rect.x * sbpp;
		uint8_t* line_dest = dest + (dst_rect.y + y) * rpitch + dst_rect.x * rbpp;
		std::size_t size = src_rect.w + src_rect.x <= surf->w ? src_rect.w : surf->w - src_rect.x;

		assert(rpitch >= src_rect.w * rbpp);
		memcpy(line_dest, line_src, size * rbpp);
	}

	return res;
}
surface blend_surface(
		  const surface &surf
		, const double amount
		, const color_t color)
{
	if(surf== nullptr) {
		return nullptr;
	}

	surface nsurf = surf.clone();

	if(nsurf == nullptr) {
		PLAIN_LOG << "could not make neutral surface...";
		return nullptr;
	}

	{
		surface_lock lock(nsurf);
		uint32_t* beg = lock.pixels();
		uint32_t* end = beg + nsurf->w*surf->h;

		uint16_t ratio = amount * 256;
		const uint16_t red   = ratio * color.r;
		const uint16_t green = ratio * color.g;
		const uint16_t blue  = ratio * color.b;
		ratio = 256 - ratio;

		while(beg != end) {
			uint8_t a = static_cast<uint8_t>(*beg >> 24);
			uint8_t r = (ratio * static_cast<uint8_t>(*beg >> 16) + red)   >> 8;
			uint8_t g = (ratio * static_cast<uint8_t>(*beg >> 8)  + green) >> 8;
			uint8_t b = (ratio * static_cast<uint8_t>(*beg)       + blue)  >> 8;

			*beg = (a << 24) | (r << 16) | (g << 8) | b;

			++beg;
		}
	}

	return nsurf;
}

/* Simplified RotSprite algorithm.
 * http://en.wikipedia.org/wiki/Image_scaling#RotSprite
 * Lifted from: http://github.com/salmonmoose/SpriteRotator
 * 1) Zoom the source image by a certain factor.
 * 2) Scan the zoomed source image at every step=offset and put it in the result. */
surface rotate_any_surface(const surface& surf, float angle, int zoom, int offset)
{
	int src_w, src_h, dst_w, dst_h;
	float min_x, min_y, sine, cosine;
	{
		float max_x, max_y;
		// convert angle to radiant (angle * 2 * PI) / 360
		const float radians = angle * boost::math::constants::pi<float>() / 180;
		cosine = std::cos(radians);
		sine   = std::sin(radians);
		// calculate the size of the dst image
		src_w = surf->w * zoom;
		src_h = surf->h * zoom;
		/* See http://en.wikipedia.org/wiki/Rotation_(mathematics) */
		const float point_1x = src_h * -sine;
		const float point_1y = src_h * cosine;
		const float point_2x = src_w * cosine - src_h * sine;
		const float point_2y = src_h * cosine + src_w * sine;
		const float point_3x = src_w * cosine;
		const float point_3y = src_w * sine;
		/* After the rotation, the new image has different dimensions.
		 * E.g.: The maximum height equals the former diagonal in case the angle is 45, 135, 225 or 315 degree.
		 * See http://en.wikipedia.org/wiki/File:Rotation_illustration2.svg to get the idea. */
		min_x = std::min(0.0F, std::min(point_1x, std::min(point_2x, point_3x)));
		min_y = std::min(0.0F, std::min(point_1y, std::min(point_2y, point_3y)));
		max_x = (angle >  90 && angle < 180) ? 0 : std::max(point_1x, std::max(point_2x, point_3x));
		max_y = (angle > 180 && angle < 270) ? 0 : std::max(point_1y, std::max(point_2y, point_3y));
		dst_w = static_cast<int>(ceil(std::abs(max_x) - min_x)) / zoom;
		dst_h = static_cast<int>(ceil(std::abs(max_y) - min_y)) / zoom;
	}
	surface dst(dst_w, dst_h);
	{
		surface_lock dst_lock(dst);
		const surface src = scale_surface(surf, src_w, src_h);
		const_surface_lock src_lock(src);
		const float scale =   1.f / zoom;
		const int   max_x = dst_w * zoom;
		const int   max_y = dst_h * zoom;
		/* Loop through the zoomed src image,
		 * take every pixel in steps with offset distance and place it in the dst image. */
		for (int x = 0; x < max_x; x += offset)
			for (int y = 0; y < max_y; y += offset) {
				// calculate the src pixel that fits in the dst
				const float source_x = (x + min_x)*cosine + (y + min_y)*sine;
				const float source_y = (y + min_y)*cosine - (x + min_x)*sine;
				// if the pixel exists on the src surface
				if (source_x >= 0 && source_x < src_w
						&& source_y >= 0 && source_y < src_h)
					// get it from the src surface and place it on the dst surface
					put_pixel(dst, dst_lock, x*scale , y*scale, // multiply with scale
							get_pixel(src, src_lock, source_x, source_y));
			}
	}

	return dst;
}

void put_pixel(const surface& surf, surface_lock& surf_lock, int x, int y, uint32_t pixel)
{
	const int bpp = surf->format->BytesPerPixel;
	/* dst is the address to the pixel we want to set */
	uint8_t* const dst = reinterpret_cast<uint8_t*>(surf_lock.pixels()) + y * surf->pitch + x * bpp;
	switch (bpp) {
	case 1:
		*dst = pixel;
		break;
	case 2:
		*reinterpret_cast<uint16_t*>(dst) = pixel;
		break;
	case 3:
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
		dst[0] = (pixel >> 16) & 0xff;
		dst[1] = (pixel >> 8) & 0xff;
		dst[2] = pixel & 0xff;
#else
		dst[0] = pixel & 0xff;
		dst[1] = (pixel >> 8) & 0xff;
		dst[2] = (pixel >> 16) & 0xff;
#endif
		break;
	case 4:
		*reinterpret_cast<uint32_t*>(dst) = pixel;
		break;
	default:
		break;
	}
}

uint32_t get_pixel(const surface& surf, const const_surface_lock& surf_lock, int x, int y)
{
	const int bpp = surf->format->BytesPerPixel;
	/* p is the address to the pixel we want to retrieve */
	const uint8_t* const src = reinterpret_cast<const uint8_t*>(surf_lock.pixels()) + y * surf->pitch + x * bpp;
	switch (bpp) {
	case 1:
		return *src;
	case 2:
		return *reinterpret_cast<const uint16_t*>(src);
	case 3:
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
		return src[0] << 16 | src[1] << 8 | src[2];
#else
		return src[0] | src[1] << 8 | src[2] << 16;
#endif
	case 4:
		return *reinterpret_cast<const uint32_t*>(src);
	}
	return 0;
}

// Rotates a surface 180 degrees.
surface rotate_180_surface(const surface &surf)
{
	if ( surf == nullptr )
		return nullptr;

	// Work with a "neutral" surface.
	surface nsurf = surf.clone();

	if ( nsurf == nullptr ) {
		PLAIN_LOG << "could not make neutral surface...";
		return nullptr;
	}

	{// Code block to limit the scope of the surface lock.
		surface_lock lock(nsurf);
		uint32_t* const pixels = lock.pixels();

		// Swap pixels in the upper half of the image with
		// those in the lower half.
		for (int y=0; y != nsurf->h/2; ++y) {
			for(int x=0; x != nsurf->w; ++x) {
				const int index1 = y*nsurf->w + x;
				const int index2 = (nsurf->h-y)*nsurf->w - x - 1;
				std::swap(pixels[index1],pixels[index2]);
			}
		}

		if ( is_odd(nsurf->h) ) {
			// The middle row still needs to be processed.
			for (int x=0; x != nsurf->w/2; ++x) {
				const int index1 = (nsurf->h/2)*nsurf->w + x;
				const int index2 = (nsurf->h/2)*nsurf->w + (nsurf->w - x - 1);
				std::swap(pixels[index1],pixels[index2]);
			}
		}
	}

	return nsurf;
}


// Rotates a surface 90 degrees, either clockwise or counter-clockwise.
surface rotate_90_surface(const surface &surf, bool clockwise)
{
	if ( surf == nullptr )
		return nullptr;

	surface dst(surf->h, surf->w); // Flipped dimensions.

	if ( surf == nullptr  ||  dst == nullptr ) {
		PLAIN_LOG << "could not make neutral surface...";
		return nullptr;
	}

	{// Code block to limit the scope of the surface locks.
		const_surface_lock src_lock(surf);
		surface_lock dst_lock(dst);

		const uint32_t* const src_pixels = src_lock.pixels();
		uint32_t* const dst_pixels = dst_lock.pixels();

		// Copy the pixels.
		for(int y = 0; y != surf->h; ++y) {
			for ( int x = 0; x != surf->w; ++x ) {
				const int src_index = y*surf->w + x;
				const int dst_index = clockwise ?
				                          x*dst->w + (dst->w-1-y) :
				                          (dst->h-1-x)*dst->w + y;
				dst_pixels[dst_index] = src_pixels[src_index];
			}
		}
	}

	return dst;
}


surface flip_surface(const surface &surf)
{
	if(surf == nullptr) {
		return nullptr;
	}

	surface nsurf = surf.clone();

	if(nsurf == nullptr) {
		PLAIN_LOG << "could not make neutral surface...";
		return nullptr;
	}

	{
		surface_lock lock(nsurf);
		uint32_t* const pixels = lock.pixels();

		for(int y = 0; y != nsurf->h; ++y) {
			for(int x = 0; x != nsurf->w/2; ++x) {
				const int index1 = y*nsurf->w + x;
				const int index2 = (y+1)*nsurf->w - x - 1;
				std::swap(pixels[index1],pixels[index2]);
			}
		}
	}

	return nsurf;
}

surface flop_surface(const surface &surf)
{
	if(surf == nullptr) {
		return nullptr;
	}

	surface nsurf = surf.clone();

	if(nsurf == nullptr) {
		PLAIN_LOG << "could not make neutral surface...";
		return nullptr;
	}

	{
		surface_lock lock(nsurf);
		uint32_t* const pixels = lock.pixels();

		for(int x = 0; x != nsurf->w; ++x) {
			for(int y = 0; y != nsurf->h/2; ++y) {
				const int index1 = y*nsurf->w + x;
				const int index2 = (nsurf->h-y-1)*surf->w + x;
				std::swap(pixels[index1],pixels[index2]);
			}
		}
	}

	return nsurf;
}

surface get_surface_portion(const surface &src, SDL_Rect &area)
{
	if (src == nullptr) {
		return nullptr;
	}

	// Check if there is something in the portion
	if(area.x >= src->w || area.y >= src->h || area.x + area.w < 0 || area.y + area.h < 0) {
		return nullptr;
	}

	if(area.x + area.w > src->w) {
		area.w = src->w - area.x;
	}
	if(area.y + area.h > src->h) {
		area.h = src->h - area.y;
	}

	// use same format as the source (almost always the screen)
	surface dst(area.w, area.h);

	if(dst == nullptr) {
		PLAIN_LOG << "Could not create a new surface in get_surface_portion()";
		return nullptr;
	}

	// Blit to dst with BLENDMODE_NONE, then reset src blend mode.
	SDL_BlendMode src_blend;
	SDL_GetSurfaceBlendMode(src, &src_blend);
	SDL_SetSurfaceBlendMode(src, SDL_BLENDMODE_NONE);
	SDL_BlitSurface(src, &area, dst, nullptr);
	SDL_SetSurfaceBlendMode(src, src_blend);

	return dst;
}

namespace {

struct not_alpha
{
	not_alpha() {}

	// we assume neutral format
	bool operator()(uint32_t pixel) const {
		uint8_t alpha = pixel >> 24;
		return alpha != 0x00;
	}
};

}
surface get_non_transparent_portion(const surface &surf)
{
	if(surf == nullptr)
		return nullptr;

	surface nsurf = surf.clone();
	if(nsurf == nullptr) {
		PLAIN_LOG << "failed to make neutral surface";
		return nullptr;
	}

	SDL_Rect res {0,0,0,0};
	const not_alpha calc;

	surface_lock lock(nsurf);
	const uint32_t* const pixels = lock.pixels();

	int n;
	for(n = 0; n != nsurf->h; ++n) {
		const uint32_t* const start_row = pixels + n*nsurf->w;
		const uint32_t* const end_row = start_row + nsurf->w;

		if(std::find_if(start_row,end_row,calc) != end_row)
			break;
	}

	res.y = n;

	for(n = 0; n != nsurf->h-res.y; ++n) {
		const uint32_t* const start_row = pixels + (nsurf->h-n-1)*surf->w;
		const uint32_t* const end_row = start_row + nsurf->w;

		if(std::find_if(start_row,end_row,calc) != end_row)
			break;
	}

	// The height is the height of the surface,
	// minus the distance from the top and
	// the distance from the bottom.
	res.h = nsurf->h - res.y - n;

	for(n = 0; n != nsurf->w; ++n) {
		int y;
		for(y = 0; y != nsurf->h; ++y) {
			const uint32_t pixel = pixels[y*nsurf->w + n];
			if(calc(pixel))
				break;
		}

		if(y != nsurf->h)
			break;
	}

	res.x = n;

	for(n = 0; n != nsurf->w-res.x; ++n) {
		int y;
		for(y = 0; y != nsurf->h; ++y) {
			const uint32_t pixel = pixels[y*nsurf->w + surf->w - n - 1];
			if(calc(pixel))
				break;
		}

		if(y != nsurf->h)
			break;
	}

	res.w = nsurf->w - res.x - n;

	surface cropped = get_surface_portion(nsurf, res);
	if(cropped && res.w > 0 && res.h > 0) {
		surface scaled = scale_surface(cropped, res.w, res.h);
		if(scaled) {
			return scaled;
		}
	}

	ERR_DP << "Failed to either crop or scale the surface";
	return nsurf;
}
