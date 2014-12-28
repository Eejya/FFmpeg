/*
 * Copyright (c) 2014 Eejya Singh
 *
 * This file is part of FFmpeg.
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

/**
 * @file
 * DVD 1.1 capture device for libavdevice
 **/

#include "libavutil/opt.h"
#include "libavutil/time.h"
#include "libavformat/internal.h"
#include "avdevice.h"

#include <dvdnav/dvdnav.h>
#include <dvdnav/dvd_types.h>


/* libdvdnav's read ahead cache to be used */
#define DVD_READ_CACHE 1

/* default language for menus/audio/subpictures */
#define DVD_LANGUAGE "en"

typedef struct {
    dvdnav_t   *nav_data;
    uint8_t    cache_buf[DVD_VIDEO_LB_LEN];
    char       *language;
    dvdnav_status_t status;
    /*Sample Rate of the audio in the DVD (in Hz) */
    int        sample_rate; 
} DVDContext;


static int read_header(AVFormatContext *ctx)
{
	AVStream *st_audio = NULL;
	AVStream *st_video = NULL;
	AVStream *st_subp = NULL;
    int audio_stream,spu_stream;
    DVDContext *dvd = ctx->priv_data;
	uint32_t width = 0 , height = 0, i_width, i_height;
	uint8_t stream,stream_format;
	uint8_t mem[DVD_VIDEO_LB_LEN];
	int dump = 0, tt_dump = 0, finished = 0;
	int result, event, len;
	uint8_t *buf = mem;
	
    if((dvd->status = dvdnav_open(&dvd->nav_data,ctx->filename[0] ? ctx->filename : NULL)) != DVDNAV_STATUS_OK)
    {
        return AVERROR(EIO);
    }
    dvd->language = av_strdup(DVD_LANGUAGE);

    /* set read ahead cache usage */
    if (dvdnav_set_readahead_flag(dvd->nav_data, DVD_READ_CACHE) != DVDNAV_STATUS_OK) {
        dvdnav_close(dvd->nav_data);
        return AVERROR(EACCES);
    }

    /* set the language */
    if ((dvdnav_menu_language_select (dvd->nav_data, dvd->language) != DVDNAV_STATUS_OK)  ||
        (dvdnav_audio_language_select(dvd->nav_data, dvd->language) != DVDNAV_STATUS_OK)  ||
        (dvdnav_spu_language_select  (dvd->nav_data, dvd->language) != DVDNAV_STATUS_OK)) {
        av_log(ctx, AV_LOG_ERROR, "Error selecting language\n");
        return AVERROR(EACCES);
    }
   
	
    /*set the PGC positioning flag to have position information relatively to the
     *current chapter (seek will seek in the chapter) */
    if (dvdnav_set_PGC_positioning_flag(dvd->nav_data, 0) != DVDNAV_STATUS_OK) {
        av_log(ctx, AV_LOG_ERROR, "Error setting PGC positioning flags\n");
        dvdnav_close(dvd->nav_data);
        return AVERROR(EACCES);
    }

	while (!finished) {
	    
	    /* the main reading function */
		#if DVD_READ_CACHE
	    	result = dvdnav_get_next_cache_block(dvd->nav_data, &buf, &event, &len);
		#else
	    	result = dvdnav_get_next_block(dvd->nav_data, buf, &event, &len);
		#endif
	    if (result == DVDNAV_STATUS_ERR) {
	      printf("Error getting next block: %s\n", dvdnav_err_to_string(dvd->nav_data));
	      return 3;
	    }
	    switch (event) {
    case DVDNAV_BLOCK_OK:
      /* We have received a regular block of the currently playing MPEG stream.
       * A real player application would now pass this block through demuxing
       * and decoding. We simply write it to disc here. */
     break;
    case DVDNAV_NOP:
      /* Nothing to do here. */
      break;
    case DVDNAV_STILL_FRAME: 
      /* We have reached a still frame. A real player application would wait
       * the amount of time specified by the still's length while still handling
       * user input to make menus and other interactive stills work.
       * A length of 0xff means an indefinite still which has to be skipped
       * indirectly by some user interaction. */
      {
      dvdnav_still_event_t *still_event = (dvdnav_still_event_t *)buf;
      if (still_event->length < 0xff)
        av_log(ctx, 0 ,"Skipping %d seconds of still frame\n", still_event->length);
      else
        av_log(ctx, 0 ,"Skipping indefinite length still frame\n");
      dvdnav_still_skip(dvd->nav_data);
      }
      break;
    case DVDNAV_WAIT:
      /* We have reached a point in DVD playback, where timing is critical.
       * Player application with internal fifos can introduce state
       * inconsistencies, because libdvdnav is always the fifo's length
       * ahead in the stream compared to what the application sees.
       * Such applications should wait until their fifos are empty
       * when they receive this type of event. */
      {
      av_log(ctx, 0 ,"Skipping wait condition\n");
      dvdnav_wait_skip(dvd->nav_data);
      }
      break;
    case DVDNAV_SPU_CLUT_CHANGE:
      /* Player applications should pass the new colour lookup table to their
       * SPU decoder */
      break;
    case DVDNAV_SPU_STREAM_CHANGE:
      /* Player applications should inform their SPU decoder to switch channels */
      av_log(ctx, 0 ,"SubPicture Stream Change \n");
      break;
    case DVDNAV_AUDIO_STREAM_CHANGE:
      /* Player applications should inform their audio decoder to switch channels */
      av_log(ctx, 0 ,"Audio Stream Change \n");
      break;
    case DVDNAV_HIGHLIGHT:
      /* Player applications should inform their overlay engine to highlight the 
       * given button */
      {
      dvdnav_highlight_event_t *highlight_event = (dvdnav_highlight_event_t *)buf;
      av_log(ctx , 0 ,"Selected button %d\n", highlight_event->buttonN);
      }
      break;
    case DVDNAV_VTS_CHANGE:
      /* Some status information like video aspect and video scale permissions do
       * not change inside a VTS. Therefore this event can be used to query such
       * information only when necessary and update the decoding/displaying
       * accordingly. */
 {
    if( dvdnav_get_video_resolution(dvd->nav_data,&i_width, &i_height ) )
      	i_width = i_height = 0;
	switch( dvdnav_get_video_aspect(dvd->nav_data) )
	{
	case 0:
		height = 4 * i_height;
		width = 3 * i_width;
		break;
	case 3:
		height = 16 * i_height;
		width = 9 * i_width;
		break;
	default:
		height = 0;
		width = 0;
		break;
	}
	}

      break;
    case DVDNAV_CELL_CHANGE:
      /* Some status information like the current Title and Part numbers do not
       * change inside a cell. Therefore this event can be used to query such
       * information only when necessary and update the decoding/displaying
       * accordingly. */
      {
      int tt = 0, ptt = 0, pos, len;   //Title and part number
      char input = '\0';
      
      dvdnav_current_title_info(dvd->nav_data, &tt, &ptt);
      dvdnav_get_position(dvd->nav_data, &pos, &len);
      av_log(ctx , 0 ,"Cell change: Title %d, Chapter %d\n", tt, ptt);
      av_log(ctx , 0 ,"At position %.0f%% inside the feature\n", 100 * (double)pos / (double)len);

      dump = 0;
      if (tt_dump && tt != tt_dump)
        tt_dump = 0;

      if (!dump && !tt_dump) {
		dump=1;		
//      av_log(ctx , 0 ,"(a)ppend cell to output\n(s)kip cell\nappend until end of (t)itle\n(q)uit\n");
//      input='a';  	
/*      switch (input) {
        case 'a':
          dump = 1;
          av_log(ctx,0,"Input is appending cell to output\n");
          break;
        case 't':
          tt_dump = tt;
          av_log(ctx,0,"Skipping current cell\n");
          break;
        case 'q':
          finished = 1;
          av_log(ctx,0,"Quit\n");
        }
*/
      }
      }
      break;
    case DVDNAV_NAV_PACKET:
      /* A NAV packet provides PTS discontinuity information, angle linking information and
       * button definitions for DVD menus. Angles are handled completely inside libdvdnav.
       * For the menus to work, the NAV packet information has to be passed to the overlay
       * engine of the player so that it knows the dimensions of the button areas. */
      {
      pci_t *pci;
      dsi_t *dsi;
      
      /* Applications with fifos should not use these functions to retrieve NAV packets,
       * they should implement their own NAV handling, because the packet you get from these
       * functions will already be ahead in the stream which can cause state inconsistencies.
       * Applications with fifos should therefore pass the NAV packet through the fifo
       * and decoding pipeline just like any other data. */
      pci = dvdnav_get_current_nav_pci(dvd->nav_data);
      dsi = dvdnav_get_current_nav_dsi(dvd->nav_data);
      
      if(pci->hli.hl_gi.btn_ns > 0) {
        int button;
        
        av_log(ctx, 0 ,"Found %i DVD menu buttons...\n", pci->hli.hl_gi.btn_ns);

        for (button = 0; button < pci->hli.hl_gi.btn_ns; button++) {
          btni_t *btni = &(pci->hli.btnit[button]);
          av_log(ctx, 0 ,"Button %i top-left @ (%i,%i), bottom-right @ (%i,%i)\n", 
                button + 1, btni->x_start, btni->y_start,
                btni->x_end, btni->y_end);
        }

        button = 0;
        while ((button <= 0) || (button > pci->hli.hl_gi.btn_ns)) {
          av_log(ctx, 0 ,"Which button (1 to %i): ", pci->hli.hl_gi.btn_ns);
          //scanf("%i", &button);
          button++;
        }

        av_log(ctx, 0 ,"Selecting button %i...\n", button);
        /* This is the point where applications with fifos have to hand in a NAV packet
         * which has traveled through the fifos. See the notes above. */
        dvdnav_button_select_and_activate(dvd->nav_data, pci, button);
      }
      }
      break;
    case DVDNAV_HOP_CHANNEL:
      /* This event is issued whenever a non-seamless operation has been executed.
       * Applications with fifos should drop the fifos content to speed up responsiveness. */
      break;
    case DVDNAV_STOP:
      /* Playback should end here. */
      {
      	finished = 1;
      	av_log(ctx, 0 ,"Stopping Playback\n");
      }
      break;
    default:
      av_log(ctx, 0 ,"Unknown event (%i)\n", event);
      finished = 1;
      break;
}

	#if DVD_READ_CACHE
    	if(dvdnav_free_cache_block(dvd->nav_data, buf) != DVDNAV_STATUS_OK)
    		{
    		av_log(ctx,0,"Error freeing the buffer");
    		return 5;
    		}
	#endif
	
}
av_log(ctx,0,"Finished !");   
    /*Setting the codec IDs and other attributes*/
/*
    st_video = avformat_new_stream(ctx, NULL);	
	st_audio = avformat_new_stream(ctx, NULL);
	st_subp = avformat_new_stream(ctx, NULL);	

    if (!st_video)
       return AVERROR(ENOMEM);
    avpriv_set_pts_info(st_video, 64, 1, 100);   //pts information in seconds at max?? 
    st_video->codec->codec_type = AVMEDIA_TYPE_VIDEO;
    st_video->codec->codec_id   = AV_CODEC_ID_MPEG2VIDEO;
    st_video->codec->width = width;
    st_video->codec->height = height;
	av_log(ctx,0,"Height and Width are %d and %d\n", height , width);

    if (!st_audio)
       return AVERROR(ENOMEM);
    avpriv_set_pts_info(st_audio, 64, 1, 100);    
    st_audio->codec->codec_type  = AVMEDIA_TYPE_AUDIO;
    st_audio->codec->codec_id    = AV_CODEC_ID_AC3;

	av_log(ctx,0,"Audio Check !");
    if (!st_subp)
       return AVERROR(ENOMEM);
    avpriv_set_pts_info(st_subp, 64, 1, 100);   
    st_subp->codec->codec_type = AVMEDIA_TYPE_SUBTITLE;
    st_subp->codec->codec_id   = AV_CODEC_ID_DVD_SUBTITLE;
  	av_log(ctx,0,"Subp Check !");
*/ 
    return 0;
}

static int read_packet(AVFormatContext* ctx, AVPacket *pkt)
{
    return pkt->size;
}

static int read_close(AVFormatContext* ctx)
{
    av_log(ctx,0,"Closing the DVD structure"); 
    DVDContext *dvd = (DVDContext *)ctx->priv_data;
    if (dvd->status && dvd->nav_data != NULL) {
        dvdnav_close(dvd->nav_data);
    }
    return 0;
}

static const AVClass class = {
    .class_name = "dvd",
    .item_name = av_default_item_name,
//    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
    .category = AV_CLASS_CATEGORY_DEVICE_AUDIO_INPUT,
};

AVInputFormat ff_dvd_demuxer = {
    .name = "dvd",
    .long_name = NULL_IF_CONFIG_SMALL("DVD audio video capture device"),
    .priv_data_size = sizeof(DVDContext),
    .read_probe = NULL,
    .read_header = read_header,
    .read_packet = read_packet,
    .read_close = read_close,
    .flags = AVFMT_NOFILE,
    .priv_class = &class
};
