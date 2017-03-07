/*spffdec.c
Authors Adam Waggoner and Jaden Holladay

Contains functionality for decoding .spff files

EXPLANATION OF COMPRESSION:
 Our compression method uses the same trick computers monitors use to display
 color on screen on a bigger scale in an image. Monitors display pixels using 3 
 leds colored red, green and blue. Since the human eye can't discern the individual
 lights, the result appears to be a solid color.
 
 Our encoder takes pixel information into a 24 bit RGB value with 8 bits per channel. 
 For each pixel, we only store one of the three channels, effectively cutting the
 file size of an image down to 1 byte per pixel. Sice pixels are generally small in
 large images, this means that the human eye averages contiguous pixels and can 
 reconstruct a fairly recognizable image.
 
 The decoder uses data from adjacent pixels to get an average value for the two
 channels that weren't stored, so each pixel has a similar color value to the 
 original image. This works because in most images (especially pictures) contiguous
 pixels are usually very similar.

CODE CITATIONS:
 We based our encoder and decoder on bmp.c and bmpenc.c from the ffmpeg source code

 Our code to convert pixel format was based on this stackoverflow post 
 http://stackoverflow.com/questions/12831761/how-to-resize-a-picture-using-ffmpegs-sws-scale
*/

#include <inttypes.h>

#include "avcodec.h"
#include "bytestream.h"
#include "spff.h" 
#include "spffdec.h"
#include "internal.h"
#include "msrledec.h"

//Returns the RGB values of a pixel at a certain coordinate position in src
static RGBValues get_rgb_pos(const uint8_t* src, int width, int row, int x)
{
	//Calculate the pixel's position in src memory
    src += row * width;
    src += x;
	
	//Initialize an empty rgb values struct
    RGBValues rgb;
    rgb.Red = 0;
    rgb.Green = 0;
    rgb.Blue = 0;

	//Returns the available channel at that row (See explanation of compression)
    switch((x + (row % 2)*2) % 3)
    {
        case 0:
	        rgb.Red = *src;
	        break;
        case 1:
	        rgb.Green = *src;
	        break;
        case 2:
	        rgb.Blue = *src;
	        break;
    }
   
   return rgb;
}

//Returns an average of RGB pixel values, ignoring empty channels
static RGBValues avg_rgb(RGBValues first, RGBValues second)
{
    RGBValues rgb = first;

	if (second.Red > 0)
	{
		rgb.Red   = (uint8_t)(((double)first.Red + (double)second.Red) /(double)2);
	}
	if (second.Green > 0)
	{
		rgb.Green = (uint8_t)(((double)first.Green + (double)second.Green) /(double)2);
	}
	if (second.Blue > 0)
	{
		rgb.Blue   = (uint8_t)(((double)first.Blue + (double)second.Blue) /(double)2);
	}
	return rgb;
}

//Returns a pixel's color averaged with its adjacent pixels
static RGBValues get_rgb_avg(const uint8_t* src, int width, int height, int row, int x)
{
    RGBValues rgb;
    rgb.Red = 0;
    rgb.Green = 0;
    rgb.Blue = 0;

	if (x > 0)
	{
		RGBValues left = get_rgb_pos(src, width, row, x-1);
		rgb = avg_rgb(rgb, left);
		if (row > 0)
		{
		    RGBValues top_left = get_rgb_pos(src, width, row-1, x-1);
			rgb = avg_rgb(rgb, top_left);
		}
		if (row < height)
		{
			RGBValues bot_left = get_rgb_pos(src, width, row+1, x-1);
			rgb = avg_rgb(rgb, bot_left);
		}
	}

	if (x < width)
	{
		RGBValues right = get_rgb_pos(src, width, row, x+1);
		rgb = avg_rgb(rgb, right);
		if (row > 0)
		{
		    RGBValues top_right = get_rgb_pos(src, width, row-1, x+1);
			rgb = avg_rgb(rgb, top_right);
		}
		if (row < height)
		{
			RGBValues bot_right = get_rgb_pos(src, width, row+1, x+1);
			rgb = avg_rgb(rgb, bot_right);
		}
	}

	if (row > 0)
	{
		RGBValues top = get_rgb_pos(src, width, row-1, x);
		rgb = avg_rgb(rgb, top);
	}

	if (row < height)
	{
		RGBValues bot = get_rgb_pos(src, width, row+1, x);
		rgb = avg_rgb(rgb, bot);
	}
	
	RGBValues rgbPixel = get_rgb_pos(src, width, row, x);
	if (rgbPixel.Red > 0 )
	{
		rgb.Red = rgbPixel.Red;
	}
	if (rgbPixel.Green > 0 )
	{
		rgb.Green = rgbPixel.Green;
	}
	if (rgbPixel.Blue > 0 )
	{
		rgb.Blue = rgbPixel.Blue;
	}
	
	return rgb;
}

static int spff_decode_frame(AVCodecContext *avctx,
                            void *data, int *got_frame,
                            AVPacket *avpkt)
{
	
    const uint8_t *buf = avpkt->data;
    int buf_size       = avpkt->size;
    AVFrame *p         = data;
    int width, height, imgsize;
    int linesize, ret;
    uint8_t *ptr;
    const uint8_t *buf0 = buf;

	//The size of the header in bytes 
	const int HEADER_SIZE = 8;

    if (avpkt->size <= HEADER_SIZE) {
        av_log(avctx, AV_LOG_ERROR, "buf size too small (%d)\n", buf_size);
        return AVERROR_INVALIDDATA;
    }

	//Read the file header info  
	width  = bytestream_get_le32(&buf);    //Width
    height = bytestream_get_le32(&buf);    //Height

	//Set the dimensions for the AVFrame
    avctx->width  = width;
    avctx->height = height;
	//Set pixel format
    avctx->pix_fmt = AV_PIX_FMT_RGB24;

	//Allocate space for the image
    if ((ret = ff_get_buffer(avctx, p, 0)) < 0)
        return ret;

    buf = buf0 + HEADER_SIZE;

	//Set pointer
    ptr      = p->data[0];
    linesize = p->linesize[0];

	const uint8_t *src = (const uint8_t *) buf;

	//iterate over each byte in the file
	for (int i = 0; i < avctx->height; i++) {
		RGBValues *dst = (RGBValues *) ptr;
	    
		for (int j = 0; j < avctx->width; j++)
		{
			*dst = get_rgb_avg(src, width, height, i, j);
			dst++;
		}
		
        ptr += linesize;
    }
	
	//Signal to ffmpeg that the image has been decoded
    *got_frame = 1;

    return buf_size;
}

AVCodec ff_spff_decoder = {
    .name           = "spff",
    .long_name      = NULL_IF_CONFIG_SMALL("SPFF image (a project for CS 3505)"),
    .type           = AVMEDIA_TYPE_VIDEO,
    .id             = AV_CODEC_ID_SPFF,
    .decode         = spff_decode_frame,
    .capabilities   = AV_CODEC_CAP_DR1,
};
