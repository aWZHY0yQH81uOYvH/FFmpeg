/*
 * This filter tries to convert lecture videos into dark mode
 *
 * The filter looks for a large square of bright pixels in the middle of
 * the frame (the lecture presentation), makes everything else black, then
 * inverts the brightness of those pixels in the middle, making sure to not
 * leave large bright areas in the resulting image from dark things in the
 * input. Colors are converted into HSV before inverting so the colors don't
 * get all gross. There are a few options that let you control how everything
 * works, described in darkmode_options below.
 *
 * To use this you'll need to compile ffmpeg from source and add references
 * to this file to a few places as described here:
 * https://fossies.org/linux/ffmpeg/doc/writing_filters.txt
 *    Add "OBJS-$(CONFIG_DARKMODE_FILTER) += vf_darkmode.o" to libavfilter/Makefile
 *    Add "extern AVFilter ff_vf_darkmode;" to libavfilter/allfilters.c
 * This works with FFmpeg version 7.1.1; probably will immediately break
 * for any other version.
 *
 * I couldn't be bothered to make this multithreaded so it hurts encode
 * speed a bit. Works fine in ffplay though.
 *
 * If exporting as an mp4 or other formats, you'll probably have to specify
 * -pix_fmt yuv420p so it's playable (this filter only supports RGB color so
 * I guess it autoconverts the color space)
 *
 * IDK how to do licenses. Do whatever the hell you want with this.
 */

#include "libavutil/imgutils.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"
#include "avfilter.h"
#include "drawutils.h"
#include "formats.h"
#include "video.h"

typedef struct HSVColor {
	float h;
	float s;
	float v;
} HSVColor;

typedef struct DarkModeContext {
	const AVClass *class;
	
	// Storage for options
	int sidelen;
	int bgthresh;
	
	int limbright;
	int brightsize;
	int brightmax;
	
	int deletemargin;
	int margin;
	
	float colorsuppress;
	
	// Intermediate frame for limiting brightness
	uint8_t *intBuf;
	
} DarkModeContext;

#define OFFSET(x) offsetof(DarkModeContext, x)
#define FLAGS AV_OPT_FLAG_VIDEO_PARAM|AV_OPT_FLAG_FILTERING_PARAM

static const AVOption darkmode_options[] = {
	{ "sidelen",       "how many pixels must be bright along an axis to define an edge of the presentation area", OFFSET(sidelen),       AV_OPT_TYPE_INT,   {256},      0,   8192, FLAGS },
	{ "bgthresh",      "maximum brightness of the background (for presentation area detection)",                  OFFSET(bgthresh),      AV_OPT_TYPE_INT,   {64},       0,   255,  FLAGS },
	{ "brightlim",     "limit large areas of brightness of resulting image (on/off)",                             OFFSET(limbright),     AV_OPT_TYPE_BOOL,  {1},        0,   1,    FLAGS },
	{ "brightsize",    "how many pixels in cross shape to search to make sure nothing is too bright",             OFFSET(brightsize),    AV_OPT_TYPE_INT,   {16},       0,   8192, FLAGS },
	{ "brightmax",     "maximum allowed brightness in the cross area",                                            OFFSET(brightmax),     AV_OPT_TYPE_INT,   {128},      0,   255,  FLAGS },
	{ "deletemargin",  "make everything around presentation area black (on/off)",                                 OFFSET(deletemargin),  AV_OPT_TYPE_BOOL,  {1},        0,   1,    FLAGS },
	{ "margin",        "amount to inset the darkness into the presentation area",                                 OFFSET(margin),        AV_OPT_TYPE_INT,   {5},        0,   8192, FLAGS },
	{ "colorsuppress", "brightness values under which color is removed from the pixel (helps with weird colors)", OFFSET(colorsuppress), AV_OPT_TYPE_FLOAT, {.dbl=0.2}, 0.0, 1.0,  FLAGS },
	{ NULL }
};

AVFILTER_DEFINE_CLASS(darkmode);

// https://stackoverflow.com/questions/3018313/algorithm-to-convert-rgb-to-hsv-and-hsv-to-rgb-in-range-0-255-for-both

static HSVColor rgb2hsv(uint8_t *in) {
	HSVColor out;
	float min, max, delta;
	float r, g, b;
	r = in[0]/255.0;
	g = in[1]/255.0;
	b = in[2]/255.0;

	min = r < g ? r : g;
	min = min < b ? min : b;

	max = r > g ? r : g;
	max = max > b ? max : b;

	out.v = (r+g+b)/3; //max;
	delta = max - min;
	if (delta < 0.1) {
		out.s = 0;
		out.h = 0; // undefined, maybe nan?
		return out;
	}
	if(max > 0.0) // NOTE: if Max is == 0, this divide would cause a crash
		out.s = (delta / max); // s
	else {
		// if max is 0, then r = g = b = 0			  
		// s = 0, h is undefined
		out.s = 0.0;
		out.h = 0.0;
		return out;
	}
	if(r >= max) // > is bogus, just keeps compilor happy
		out.h = (g - b) / delta; // between yellow & magenta
	else
	if(g >= max)
		out.h = 2.0 + (b - r) / delta; // between cyan & yellow
	else
		out.h = 4.0 + (r - g) / delta; // between magenta & cyan

	out.h *= 60.0; // degrees

	if(out.h < 0.0)
		out.h += 360.0;

	return out;
}


static void hsv2rgb(HSVColor in, uint8_t *out) {
	float hh, p, q, t, ff;
	long i;
	float r, g, b;
	
	if(in.s <= 0.0) { // < is bogus, just shuts up warnings
		r = in.v;
		g = in.v;
		b = in.v;
		goto done;
	}
	
	hh = in.h;
	if(hh >= 360.0) hh = 0.0;
	hh /= 60.0;
	i = (long)hh;
	ff = hh - i;
	p = in.v * (1.0 - in.s);
	q = in.v * (1.0 - (in.s * ff));
	t = in.v * (1.0 - (in.s * (1.0 - ff)));

	switch(i) {
	case 0:
		r = in.v;
		g = t;
		b = p;
		goto done;
	case 1:
		r = q;
		g = in.v;
		b = p;
		goto done;
	case 2:
		r = p;
		g = in.v;
		b = t;
		goto done;
	case 3:
		r = p;
		g = q;
		b = in.v;
		goto done;
	case 4:
		r = t;
		g = p;
		b = in.v;
		goto done;
	case 5:
	default:
		r = in.v;
		g = p;
		b = q;
		goto done;
	}
	
	done:
	out[0]=r*255;
	out[1]=g*255;
	out[2]=b*255;
}

static inline int brightness(uint8_t *ptr) {
	return ((int)ptr[0]+ptr[1]+ptr[2])/3;
}

// Inefficiently process a frame
static void darkmode(DarkModeContext *s, AVFrame *in, AVFrame *out) {
	
	int startX = 0, stopX = in->width-1;
	int startY = 0, stopY = in->height-1;
	
	uint8_t *inptr = in->data[0] + in->height/2 * in->linesize[0];
	
	// Search in the middle of the frame for the horizontal limits of the presentation box
	// From the left
	while(startX < in->width-1) {
		if(brightness(inptr+startX*3) > s->bgthresh) {
			// Now search up and down
			int good = 1;
			for(int y=(in->height-s->sidelen)/2; y<(in->height+s->sidelen)/2; y++)
				if(brightness(in->data[0]+y*in->linesize[0]+startX*3) <= s->bgthresh) {
					good = 0;
					break;
				}
			if(good) break;
		}
		startX++;
	}
	
	// From the right
	while(stopX > 0) {
		if(brightness(inptr+stopX*3) > s->bgthresh) {
			// Now search up and down
			int good = 1;
			for(int y=(in->height-s->sidelen)/2; y<(in->height+s->sidelen)/2; y++)
				if(brightness(in->data[0]+y*in->linesize[0]+stopX*3) <= s->bgthresh) {
					good = 0;
					break;
				}
			if(good) break;
		}
		stopX--;
	}
	
	// Now search for the top and bottom
	// From the top
	while(startY < in->height-1) {
		inptr = in->data[0] + startY * in->linesize[0];
		if(brightness(inptr+in->width/2*3) > s->bgthresh) {
			// Now search left and right
			int good = 1;
			for(int x=(in->width-s->sidelen)/2; x<(in->width+s->sidelen)/2; x++)
				if(brightness(inptr+x*3) <= s->bgthresh) {
					good = 0;
					break;
				}
			if(good) break;
		}
		startY++;
	}
	
	// From the bottom
	while(stopY > 0) {
		inptr = in->data[0] + stopY * in->linesize[0];
		if(brightness(inptr+in->width/2*3) > s->bgthresh) {
			// Now search left and right
			int good = 1;
			for(int x=(in->width-s->sidelen)/2; x<(in->width+s->sidelen)/2; x++)
				if(brightness(inptr+x*3) <= s->bgthresh) {
					good = 0;
					break;
				}
			if(good) break;
		}
		stopY--;
	}
	
	if(s->deletemargin) {
		// Apply margin
		startX += s->margin;
		stopX  -= s->margin;
		startY += s->margin;
		stopY  -= s->margin;
	}
	
	// Invert colors
	if(startX < stopX && startY < stopY && startX < in->width && stopX >= 0 && startY < in->width && stopY >= 0) {
		
		// Make sure nothing is too bright
		if(s->limbright) {
			// Convert to HSV and invert V; use intBuf
			for(int y=startY; y<=stopY; y++) {
				inptr = in->data[0]  + y * in->linesize[0];
				uint8_t *outptr = s->intBuf + y * in->width * 3;
				for(int x=startX*3; x<=stopX*3; x+=3) {
					HSVColor hsv = rgb2hsv(inptr+x);
					if(hsv.v < s->colorsuppress) hsv.s = 0;
					if(hsv.v > 1.0) hsv.v = 1.0;
					hsv.v = 1.0 - hsv.v;
					hsv2rgb(hsv, outptr+x);
				}
			}
			
			// Search for bright areas and copy to output
			for(int y=startY; y<=stopY; y++) {
				inptr = s->intBuf + y * in->width * 3;
				uint8_t *outptr = out->data[0] + y * out->linesize[0];
				for(int x=startX*3; x<=stopX*3; x+=3) {
					int thisbright = brightness(inptr+x);
					if(thisbright > s->brightmax) {
						int darken1=1, darken2=1, darken3=1, darken4=1;
						for(int i = 0; i<s->brightsize; i++) {
							int x1=x+i*3, x2=x-i*3;
							int y1=y+i, y2=y-i;
							if(x1 > startX*3 && x1 < stopX*3) {
								if(y1 > startY && y1 < stopY)
									if(brightness(s->intBuf+y1*in->width*3+x1) <= s->brightmax)
										darken1=0;
								if(y2 > startY && y2 < stopY)
									if(brightness(s->intBuf+y2*in->width*3+x1) <= s->brightmax)
										darken2=0;
							}
							if(x2 > startX*3 && x2 < stopX*3) {
								if(y1 > startY && y1 < stopY)
									if(brightness(s->intBuf+y1*in->width*3+x2) <= s->brightmax)
										darken3=0;
								if(y2 > startY && y2 < stopY)
									if(brightness(s->intBuf+y2*in->width*3+x2) <= s->brightmax)
										darken4=0;
							}
						}
						
						// If two of the cross arm things didn't hit any dark pixels, we must darken the output
						if(darken1+darken2+darken3+darken4 >= 2) {
							outptr[x+0] = (int)inptr[x+0]*s->brightmax/thisbright;
							outptr[x+1] = (int)inptr[x+1]*s->brightmax/thisbright;
							outptr[x+2] = (int)inptr[x+2]*s->brightmax/thisbright;
						} else goto noreduce;
					} else noreduce: {
						outptr[x+0] = inptr[x+0];
						outptr[x+1] = inptr[x+1];
						outptr[x+2] = inptr[x+2];
					}
				}
			}
		} else {
			// Convert to HSV and invert V; don't use intBuf
			for(int y=startY; y<=stopY; y++) {
				inptr = in->data[0]  + y * in->linesize[0];
				uint8_t *outptr = out->data[0] + y * out->linesize[0];
				for(int x=startX*3; x<=stopX*3; x+=3) {
					HSVColor hsv = rgb2hsv(inptr+x);
					if(hsv.v < s->colorsuppress) hsv.s = 0;
					if(hsv.v > 1.0) hsv.v = 1.0;
					hsv.v = 1.0 - hsv.v;
					hsv2rgb(hsv, outptr+x);
				}
			}
		}
		
	} else {
		startX = in->width;
		stopX  = in->width-1;
		startY = in->height;
		stopY  = in->height-1;
	}
	
	if(s->deletemargin) {
		// Black out everything but the presentation area
		for(int y=0; y<startY; y++) {
			uint8_t *outptr = out->data[0] + y * out->linesize[0];
			for(int x=0; x<out->width*3+2; x++)
				outptr[x]=0;
		}
		for(int y=startY; y<=stopY; y++) {
			uint8_t *outptr = out->data[0] + y * out->linesize[0];
			for(int x=0; x<startX*3+2; x++)
				outptr[x]=0;
			for(int x=(stopX+1)*3; x<out->width*3+2; x++)
				outptr[x]=0;
		}
		for(int y=stopY+1; y<out->height; y++) {
			uint8_t *outptr = out->data[0] + y * out->linesize[0];
			for(int x=0; x<out->width*3+2; x++)
				outptr[x]=0;
		}
	}
	
}

// Seems like it converts to whatever you support so giv me RBG pls
static int query_formats(AVFilterContext *ctx) {
	static const enum AVPixelFormat pixel_fmts[] = {
		AV_PIX_FMT_RGB24,
		AV_PIX_FMT_NONE
	};
	
	AVFilterFormats *formats = ff_make_format_list(pixel_fmts);
	if (!formats)
		return AVERROR(ENOMEM);
	return ff_set_common_formats(ctx, formats);
}

// This gets called and does things. Just copied it from another filter
static int filter_frame(AVFilterLink *inlink, AVFrame *in) {
	AVFilterContext *ctx = inlink->dst;
	AVFilterLink *outlink = ctx->outputs[0];
	DarkModeContext *s = ctx->priv;
	AVFrame *out;
	
	// Set 'direct' if we can modify the input frame in-place. Otherwise we
	// need to retrieve a new frame from the output link.
	int direct = av_frame_is_writable(in) && !ctx->is_disabled;
	
	if(direct)
		out = in;
	else {
		out = ff_get_video_buffer(outlink, outlink->w, outlink->h);
		if(!out) {
			av_frame_free(&in);
			return AVERROR(ENOMEM);
		}
		av_frame_copy_props(out, in);
	}
	
	// Now we've got the input and output frames (which may be the same frame)
	// perform the filtering with our custom function.
	darkmode(s, in, out);
	
	if(ctx->is_disabled) {
		av_frame_free(&out);
		return ff_filter_frame(outlink, in);
	}
	
	if(!direct)
		av_frame_free(&in);
	
	return ff_filter_frame(outlink, out);
}

// Allocate wam
static int config_input(AVFilterLink *inlink) {
	DarkModeContext *s = inlink->dst->priv;
	if(s->limbright)
		s->intBuf = malloc(sizeof(uint8_t) * (inlink->w + 1) * inlink->h * 3);
	return 0;
}

// Deallocate wam
static av_cold void uninit(AVFilterContext *ctx) {
	DarkModeContext *s = ctx->priv;
	if(s->limbright)
		free(s->intBuf);
}

static const AVFilterPad inputs[] = {
	{
		.name		 = "default",
		.type		 = AVMEDIA_TYPE_VIDEO,
		.filter_frame = filter_frame,
		.config_props = config_input,
	},
	{ NULL }
};

static const AVFilterPad outputs[] = {
	{
		.name = "default",
		.type = AVMEDIA_TYPE_VIDEO,
	},
	{ NULL }
};

AVFilter ff_vf_darkmode = {
	.name            = "darkmode",
	.description     = NULL_IF_CONFIG_SMALL("Try to convert a Zoom lecture into dark mode."),
	.priv_size       = sizeof(DarkModeContext),
	.priv_class      = &darkmode_class,
	.uninit          = uninit,
	.formats         = query_formats,
	.formats_state   = FF_FILTER_FORMATS_QUERY_FUNC,
	.inputs          = inputs,
	.nb_inputs       = 1,
	.outputs         = outputs,
	.nb_outputs      = 1,
	.flags           = AVFILTER_FLAG_SUPPORT_TIMELINE_GENERIC, // why not
	.process_command = ff_filter_process_command,
};
