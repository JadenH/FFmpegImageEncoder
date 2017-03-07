/*spffenc.c
Authors: Adam Waggoner and Jaden Holladay

Contains functionality for encoding .spff files

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

#include "libavutil/imgutils.h"
#include "libavutil/avassert.h"
#include "avcodec.h"
#include "bytestream.h"
#include "internal.h"

#include <libswscale/swscale.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>

#include "spff.h"
#include "spffenc.h"

static av_cold int spff_encode_init(AVCodecContext *avctx){

    avctx->bits_per_coded_sample = 8;

    return 0;
}

//This function returns the rgb values of a pixel at a given row and column of an AVFrame
static RGBValues* get_pixel_rgb  (AVFrame const * pict, 
				   const size_t row,
				   const size_t col)
{
	return (RGBValues*)&pict->data[0][row *
					 pict->linesize[0] + col * 3];
}

static int spff_encode_frame(AVCodecContext *avctx, AVPacket *pkt,
								 const AVFrame *pict, int *got_packet)
{
    const AVFrame * const p = pict;
	
	//The size in bytes of a .spff header
	const int HEADER_SIZE = 8;

	//CONVERT TO RGB24 PIXEL FORMAT
	//TODO: cite this
	//NOTE: Colors are not exactly the same
	struct SwsContext *resize;
	resize = sws_getContext(avctx->width, avctx->height,
							avctx->pix_fmt, 
							avctx->width, avctx->height, 
							AV_PIX_FMT_RGB24,
							SWS_LANCZOS | SWS_ACCURATE_RND, NULL, NULL, NULL);
	
	AVFrame* frame2 = av_frame_alloc();
	int num_bytes = avpicture_get_size(AV_PIX_FMT_RGB24, avctx->width, avctx->height);
	
	RGBValues* frame2_buffer = (RGBValues *)av_malloc(num_bytes * sizeof(RGBValues));

	avpicture_fill((AVPicture*)frame2, frame2_buffer, 
				   AV_PIX_FMT_RGB24, avctx->width, 
				   avctx->height);

	sws_scale(resize, p->data,
			  p->linesize, 0, 
			  avctx->height, frame2->data, 
			  frame2->linesize);
    
	
    int n_bytes_image, n_bytes_per_row, ret;
    //const uint32_t *pal = NULL;
    //uint32_t palette256[256];
    //int pad_bytes_per_row, pal_entries = 0, compression = BMP_RGB;
	
	//Number of bytes in a row, since we are only storing
	//one byte per pixel, this should be equal to the image width
    n_bytes_per_row = avctx->width;

	//Bytes in the entire image
    n_bytes_image = avctx->height * (n_bytes_per_row) + HEADER_SIZE;

	//Attempt to allocate the necesarry data to store the image info, return if allocation fails
    if((ret = ff_alloc_packet2(avctx, pkt, n_bytes_image, 0)) <0)
	   return ret;

	//Pointer to destination memory
    uint8_t *buf;
	//Set buf to the address of the outgoing AVPacket's data
    buf = pkt->data;

	//Write header
    bytestream_put_le32(&buf, avctx->width);          // Width
    bytestream_put_le32(&buf, avctx->height);         // Height 
	
	//TODO: Remove these lines once we figure out why color space conversion is losing data
	RGBValues* rgb = get_pixel_rgb(frame2, 1, 1);
	printf("COLOR CHANNEL R: %d\n",rgb->Red );
    printf("COLOR CHANNEL G: %d\n",rgb->Blue );
    printf("COLOR CHANNEL B: %d\n",rgb->Green );

	//Set buffer position to first pixel
    buf = pkt->data + HEADER_SIZE;

	//Write data for each pixel
    for(int i = 0; i < avctx->height; i++) 
	{
		for(int j = 0; j < avctx->width; j++)
		{
			//The value of the color channel being written for this pixel
			uint8_t channel;
			
			/* This switch statement alternates writing red, green, blue channels for
			 * subsequent pixels. Additionally, every other row is offset and writes in
			 * the order of blue, red, green to maximize the contiguous values of
			 * each color channel.
			 */
			switch((j + (i % 2)*2) % 3)
			{
			   case 0:
				   channel = get_pixel_rgb(frame2, i, j)->Red;
				   break;
			   case 1:
				   channel = get_pixel_rgb(frame2, i, j)->Green;
				   break;
			   case 2:
				   channel = get_pixel_rgb(frame2, i, j)->Blue;
				   break; 
			}
			
			//write the appropriate color channel of this pixel to the file
			bytestream_put_byte(&buf,channel);
		}
    }
	
	pkt->flags |= AV_PKT_FLAG_KEY;
    
	//Indicates to ffmpeg that the packet is ready to be written to a file
	*got_packet = 1;

	//TODO: free memory used by avalloc?

    return 0;
}

AVCodec ff_spff_encoder = { /* SPFF description */
        .id        = AV_CODEC_ID_SPFF,
        .type      = AVMEDIA_TYPE_VIDEO,
        .name      = "spff",
        .long_name = NULL_IF_CONFIG_SMALL("SPFF image (a project for CS 3505)"),
		.init      = spff_encode_init,
		.encode2   = spff_encode_frame,
		/*.pix_fmts       = (const enum AVPixelFormat[]){
        AV_PIX_FMT_RGB8, AV_PIX_FMT_BGR8, AV_PIX_FMT_RGB4_BYTE, AV_PIX_FMT_BGR4_BYTE,
        AV_PIX_FMT_GRAY8, AV_PIX_FMT_PAL8, AV_PIX_FMT_NONE
		}*/
};
