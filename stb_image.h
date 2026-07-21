/* stb_image.h placeholder
 *
 * This project does NOT use stb. All raster I/O (PNG/JPG decode and encode,
 * plus video decode/encode) is handled by FFmpeg's libav* libraries, and PDF
 * rasterization by mupdf. Keeping raster IO in libav avoids a second image
 * dependency and means the only external C deps are ffmpeg + mupdf.
 *
 * This file exists only so old #includes fail loudly instead of pulling in a
 * partial header. If you intended to use stb, vendor the real single-header
 * implementation from https://github.com/nothings/stb and define
 * STB_IMAGE_IMPLEMENTATION in exactly one translation unit.
 */
#error "stb_image.h is not used by BadAppleStein; raster I/O goes through libav/mupdf. Remove this include."
