/*
 * rtl-sdr, turns your Realtek RTL2832 based DVB dongle into a SDR receiver
 * Copyright (C) 2012 by Steve Markgraf <steve@steve-m.de>
 * Copyright (C) 2012 by Hoernchen <la@tfc-server.de>
 * Copyright (C) 2012 by Kyle Keen <keenerd@gmail.com>
 * Copyright (C) 2013 by Elias Oenal <EliasOenal@gmail.com>
 * Copyright (C) 2015 by Hayati Ayguen <h_ayguen@web.de>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


/*
 * written because people could not do real time
 * FM demod on Atom hardware with GNU radio
 * based on rtl_sdr.c and rtl_tcp.c
 *
 * lots of locks, but that is okay
 * (no many-to-many locks)
 *
 * todo:
 *       sanity checks
 *       frequency ranges could be stored better
 *       auto-hop after time limit
 *       peak detector to tune onto stronger signals
 *       fifo for active hop frequency
 *       clips
 *       noise squelch
 *       merge stereo patch
 *       merge udp patch
 *       testmode to detect overruns
 *       watchdog to reset bad dongle
 *       fix oversampling
 */

#include <errno.h>
#include <signal.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef __APPLE__
#include <sys/time.h>
#else
#include <time.h>
#endif

#ifndef _WIN32
#include <unistd.h>
#else
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include "getopt/getopt.h"
#define usleep(x) Sleep(x/1000)
#if defined(_MSC_VER) && _MSC_VER < 1800
#define round(x) (x > 0.0 ? floor(x + 0.5): ceil(x - 0.5))
#endif
#define _USE_MATH_DEFINES
#endif

#include <math.h>
#include <pthread.h>
#include <libusb.h>

#include "rtl-sdr.h"
#include "convenience/convenience.h"

#define DEFAULT_SAMPLE_RATE		24000
#define DEFAULT_BUF_LENGTH		(1 * 16384)
#define MAXIMUM_OVERSAMPLE		16
#define MAXIMUM_BUF_LENGTH		(MAXIMUM_OVERSAMPLE * DEFAULT_BUF_LENGTH)
#define AUTO_GAIN			-100
#define DEFAULT_BUFFER_DUMP		4096

#define BUFFER_DUMP			4096
#define MAXIMUM_RATE			2400000

#define FREQUENCIES_LIMIT		1024

#define SQUELCH_TIMEOUT			300

#define PI_INT				(1<<14)
#define ONE_INT				(1<<14)

static volatile int do_exit = 0;
static int single = 0;
static int lcm_post[17] = {1,1,1,3,1,5,3,7,1,9,5,11,3,13,7,15,1};
static int ACTUAL_BUF_LENGTH;
static uint32_t MINIMUM_RATE = 1000000;

static int *atan_lut = NULL;
static int atan_lut_size = 131072; /* 512 KB */
static int atan_lut_coef = 8;
static int freqd = 0;

struct timeval time1, time2;
// rewrite as dynamic and thread-safe for multi demod/dongle
#define SHARED_SIZE 6
int16_t shared_samples[SHARED_SIZE][MAXIMUM_BUF_LENGTH];
int ss_busy[SHARED_SIZE] = {0};

enum agc_mode_t
{
	agc_off = 0,
	agc_normal,
	agc_aggressive
};
static int verbosity = 0;
static int printLevels = 0;
static int printLevelNo = 1;
static int levelMax = 0;
static int levelMaxMax = 0;
static double levelSum = 0.0;

struct dongle_state
{
	int      exit_flag;
	pthread_t thread;
	rtlsdr_dev_t *dev;
	int      dev_index;
	uint32_t userFreq;
	uint32_t freq;
	uint32_t rate;
	uint32_t bandwidth;
	int      gain;
	int16_t  *buf16;
	uint32_t buf_len;
	int      ppm_error;
	int      offset_tuning;
	int      direct_sampling;
	int      mute;
	int      pre_rotate;
	struct demod_state *targets[2];
};

struct agc_state
{
	int32_t gain_num;
	int32_t gain_den;
	int32_t gain_max;
	int     peak_target;
	int     attack_step;
	int     decay_step;
	int     error;
};

struct translate_state
{
	double angle;  /* radians */
	int16_t *sincos;  /* pairs */
	int len;
	int i;
};

struct demod_state
{
	int      exit_flag;
	pthread_t thread;
	int16_t  *lowpassed;
	int      lp_len;
	int16_t  lp_i_hist[10][6];
	int16_t  lp_q_hist[10][6];
	int16_t  droop_i_hist[9];
	int16_t  droop_q_hist[9];
	int      rate_in;
	int      rate_out;
	int      rate_out2;
	int      now_r, now_j;
	int      pre_r, pre_j;
	int      prev_index;
	int      downsample;    /* min 1, max 256 */
	int      post_downsample;
	int      output_scale;
	int      squelch_level, conseq_squelch, squelch_hits, terminate_on_squelch, squelch_timeout;
	int      squelch_timer;
	int      downsample_passes;
	int      comp_fir_size;
	int      custom_atan;
	int      deemph, deemph_a;
	int      now_lpr;
	int      prev_lpr_index;
	int      dc_block, dc_avg;
	int      log1, log2, log3, log4;
	int      ds1;
	int      rotate_enable;
	struct   translate_state rotate;
	enum	 agc_mode_t agc_mode;
	struct   agc_state *agc;
	void     (*mode_demod)(struct demod_state*);
	pthread_rwlock_t rw;
	pthread_cond_t ready;
	pthread_mutex_t ready_m;
	struct buffer_bucket *output_target;
};

struct buffer_bucket
{
	int16_t  *buf;
	int      len;
	pthread_rwlock_t rw;
	pthread_cond_t ready;
	pthread_mutex_t ready_m;
	pthread_mutex_t trycond_m;
	int      trycond;
};

struct output_state
{
	int      exit_flag;
	pthread_t thread;
	FILE     *file;
	char     *filename;
	char     *freqsfilename;
	int      freqs;
	char     *multidir;	
	struct buffer_bucket results[2];
	int      rate;
	int      wav_format;
	int      padded;
	int      lrmix;
};

typedef struct fstate
{
	uint32_t freq;
	int      squelch, conseq_squelch, squelch_timeout;
	int      gain, ppm;
	int      min_sr, max_sr, prev_sr, squelch_delta, squelch_sum, squelch_avg;
	time_t   first_squelch, last_squelch;
} freq_state;

struct controller_state
{
	int      exit_flag;
	pthread_t thread;
	int      freq_len;
	int      freq_now;
	freq_state freqs[FREQUENCIES_LIMIT];
	freq_state *freq;	
	int      edge;
	int      speedtest;
	int      wb_mode;
	pthread_cond_t hop;
	pthread_mutex_t hop_m;
};

// multiple of these, eventually
struct dongle_state dongle;
struct demod_state demod;
struct demod_state demod2;
struct output_state output;
struct controller_state controller;

void usage(void)
{
	fprintf(stderr,
		"rtl_fm, a simple narrow band FM demodulator for RTL2832 based DVB-T receivers\n\n"
		"Use:\trtl_fm -f freq [-options] [filename]\n"
		"\t-f frequency_to_tune_to [Hz]\n"
		"\t    use multiple -f for scanning (requires squelch)\n"
		"\t    ranges supported, -f 118M:137M:25k\n"
		"\t[-m minimum_capture_rate Hz (default: 1m, min=900k, max=3.2m)]\n"
		"\t[-v verbosity (default: 0)]\n"
		"\t[-M modulation (default: fm)]\n"
		"\t    fm or nbfm or nfm, wbfm or wfm, raw or iq, am, usb, lsb\n"
		"\t    wbfm == -M fm -s 170k -o 4 -A fast -r 32k -l 0 -E deemp\n"
		"\t    raw mode outputs 2x16 bit IQ pairs\n"
		"\t[-s sample_rate (default: 24k)]\n"
		"\t[-d device_index (default: 0)]\n"
                "\t[-g tuner_gain (default: automatic)]\n"
		"\t[-w tuner_bandwidth in Hz (default: automatic)]\n"
		"\t[-l squelch_level (default: 0/off)]\n"
		"\t[-L N  prints levels every N calculations]\n"
		"\t    output are comma separated values (csv):\n"
		"\t	avg rms since last output, max rms since last output, overall max rms, squelch (paramed), rms, rms level, avg rms level\n"
		"\t[-c de-emphasis_time_constant in us for wbfm. 'us' or 'eu' for 75/50 us (default: us)]\n"
		//"\t    for fm squelch is inverted\n"
		"\t[-o oversampling (default: 1, 4 recommended)]\n"
		"\t[-p ppm_error (default: 0)]\n"
		"\t[-E enable_option (default: none)]\n"
		"\t    use multiple -E to enable multiple options\n"
		"\t    edge:   enable lower edge tuning\n"
		"\t    no-dc:  disable dc blocking filter\n"
		"\t    deemp:  enable de-emphasis filter\n"
		"\t    swagc:  enable software agc (only for AM modes)\n"
		"\t    swagc-aggressive:  enable aggressive software agc (only for AM modes)\n"
		"\t    direct: enable direct sampling\n"
		"\t    no-mod: enable no-mod direct sampling\n"
		"\t    offset: enable offset tuning\n"
		"\t    log1:   enable logging frequency, signal level, date and time - squelch open\n"
		"\t    log2:   enable logging frequency, signal level, date and time - squelch closed\n"
		"\t    log3:   enable logging frequency and signal level\n"
		"\t    log4:   enable logging frequency and signal level\n"
		"\t    speedtest: enable scanning speed test\n"
		"\t    ds1: enable dynamic squelch 1\n"		
		"\t    wav:    generate WAV header\n"
		"\t    pad:    pad output gaps with zeros\n"
		"\t    lrmix:  one channel goes to left audio, one to right (broken)\n"
		"\t            remember to enable stereo (-c 2) in sox\n"
		"\t    rtlagc: enable rtl2832's digital agc (default: off)\n"
		"\tfilename ('-' means stdout)\n"
		"\t    omitting the filename also uses stdout\n\n"
		"Experimental options:\n"
		"\t[-r resample_rate (default: none / same as -s)]\n"
		"\t[-t squelch_delay (default: 10)]\n"
		"\t    +values will mute/scan, -values will exit\n"
		"\t[-F fir_size (default: off)]\n"
		"\t    enables low-leakage downsample filter\n"
		"\t    size can be 0 or 9.  0 has bad roll off\n"
		"\t[-A std/fast/lut/ale choose atan math (default: std)]\n"
		//"\t[-C clip_path (default: off)\n"
		//"\t (create time stamped raw clips, requires squelch)\n"
		//"\t (path must have '\%s' and will expand to date_time_freq)\n"
		//"\t[-H hop_fifo (default: off)\n"
		//"\t (fifo will contain the active frequency)\n"
		"\n"
		"Produces signed 16 bit ints, use Sox or aplay to hear them.\n"
		"\trtl_fm ... | play -t raw -r 24k -es -b 16 -c 1 -V1 -\n"
		"\t           | aplay -r 24k -f S16_LE -t raw -c 1\n"
		"\t  -M wbfm  | play -r 32k ... \n"
		"\t  -E wav   | play -t wav - \n"
		"\t  -s 22050 | multimon -t raw /dev/stdin\n\n");
	exit(1);
}

/* more cond dumbness */
#define safe_cond_signal(n, m) pthread_mutex_lock(m); pthread_cond_signal(n); pthread_mutex_unlock(m)
#define safe_cond_wait(n, m) pthread_mutex_lock(m); pthread_cond_wait(n, m); pthread_mutex_unlock(m)

void load_frequencies(char *filename, int reload)
{
	char line[LINE_MAX];
	char *s;
	FILE *fp;
	freq_state *f;
	
	if (reload == 1) {
		controller.freq_len = 0;
	}
	
	fp = fopen(filename, "r");
	if (fp != NULL) {
		safe_cond_signal(&controller.hop, &controller.hop_m);

		while (fgets(line, LINE_MAX, fp) != NULL)
		{			
			char *tok, *e;
			int i = 0, idx;
			s = line;

			e = strchr(s,'#');
			idx = (int)(e - s);
			
			if (idx == 0) {
				continue;
			}

			f = &controller.freqs[controller.freq_len];
			/* frequency, [squelch], [conseq_squelch], [squelch_timeout], [gain], [ppm] 
			 * * = use default value
			 */
			while((tok = strsep(&s,",")))
			{
				char *pos;
				if ((pos = strchr(tok, '*')) != NULL) {
					i++;
					continue;
				}
				if ((pos = strchr(tok, '\n')) != NULL)
					*pos = '\0';
				switch (i)
				{
					case 0:
						f->freq = (uint32_t)atofs(tok);
						break;
						
					case 1:
						f->squelch = (int)atof(tok);
						break;

					case 2:
						f->conseq_squelch = (int)atof(tok);
						break;

					case 3:
						f->squelch_timeout = (int)atof(tok);
						break;
						
					case 4:
						f->gain = (int)(atof(tok) * 10);
						break;
						
					case 5:
						f->ppm = atoi(tok);
						break;
				}
				i++;
			}
			if (i > 0)
			{
				controller.freq_len++;
			}
		}
	}
	
	fclose(fp);
}

#ifdef _WIN32
BOOL WINAPI
sighandler(int signum)
{
	if (CTRL_C_EVENT == signum) {
		fprintf(stderr, "Signal caught, exiting!\n");
		do_exit = 1;
		rtlsdr_cancel_async(dongle.dev);
		return TRUE;
	}
	return FALSE;
}
#else
static void sighandler(int signum)
{
	int i;
	freq_state *f;
	fprintf(stderr, "Signal caught, exiting!\n");
	
	if (signum == SIGUSR1) {
		fprintf(stderr, "Reload frequencies\n");
		safe_cond_signal(&controller.hop, &controller.hop_m);
		for (i = 0; i < FREQUENCIES_LIMIT; i++) {
			f = &controller.freqs[i];
			f->squelch = demod.squelch_level;
			f->conseq_squelch = demod.conseq_squelch;
			f->gain = dongle.gain;
			f->ppm = dongle.ppm_error;			
		}		
		load_frequencies(output.freqsfilename, 1);
	} else {
		fprintf(stderr, "Signal caught, exiting!\n");
		do_exit = 1;
		rtlsdr_cancel_async(dongle.dev);
	}	
}
#endif

/* {length, coef, coef, coef}  and scaled by 2^15
   for now, only length 9, optimal way to get +85% bandwidth */
#define CIC_TABLE_MAX 10
int cic_9_tables[][10] = {
	{0,},
	{9, -156,  -97, 2798, -15489, 61019, -15489, 2798,  -97, -156},
	{9, -128, -568, 5593, -24125, 74126, -24125, 5593, -568, -128},
	{9, -129, -639, 6187, -26281, 77511, -26281, 6187, -639, -129},
	{9, -122, -612, 6082, -26353, 77818, -26353, 6082, -612, -122},
	{9, -120, -602, 6015, -26269, 77757, -26269, 6015, -602, -120},
	{9, -120, -582, 5951, -26128, 77542, -26128, 5951, -582, -120},
	{9, -119, -580, 5931, -26094, 77505, -26094, 5931, -580, -119},
	{9, -119, -578, 5921, -26077, 77484, -26077, 5921, -578, -119},
	{9, -119, -577, 5917, -26067, 77473, -26067, 5917, -577, -119},
	{9, -199, -362, 5303, -25505, 77489, -25505, 5303, -362, -199},
};

#if defined(_MSC_VER) && _MSC_VER < 1800
double log2(double n)
{
	return log(n) / log(2.0);
}
#endif

void rotate_90(unsigned char *buf, uint32_t len)
/* 90 rotation is 1+0j, 0+1j, -1+0j, 0-1j
   or [0, 1, -3, 2, -4, -5, 7, -6] */
{
	uint32_t i;
	unsigned char tmp;
	for (i=0; i<len; i+=8) {
		/* uint8_t negation = 255 - x */
		tmp = 255 - buf[i+3];
		buf[i+3] = buf[i+2];
		buf[i+2] = tmp;

		buf[i+4] = 255 - buf[i+4];
		buf[i+5] = 255 - buf[i+5];

		tmp = 255 - buf[i+6];
		buf[i+6] = buf[i+7];
		buf[i+7] = tmp;
	}
}

int translate_init(struct translate_state *ts)
/* two pass: first to find optimal length, second to alloc/fill */
{
	int max_length = 100000;
	int i, s, c, best_i;
	double a, a2, err, best_360;
	if (fabs(ts->angle) < 2*M_PI/max_length) {
		fprintf(stderr, "angle too small or array too short\n");
		return 1;
	}
	ts->i = 0;
	ts->sincos = NULL;
	if (ts->len) {
		max_length = ts->len;
		ts->sincos = malloc(max_length * sizeof(int16_t));
	}
	a = 0.0;
	err = 0.0;
	best_i = 0;
	best_360 = 4.0;
	for (i=0; i < max_length; i+=2) {
		s = (int)round(sin(a + err) * (1<<14));
		c = (int)round(cos(a + err) * (1<<14));
		a2 = atan2(s, c);
		err = fmod(a, 2*M_PI) - a2;
		a += ts->angle;
		while (a > M_PI) {
			a -= 2*M_PI;}
		while (a < -M_PI) {
			a += 2*M_PI;}
		if (i != 0 && fabs(a) < best_360) {
			best_i = i;
			best_360 = fabs(a);
		}
		if (i != 0 && fabs(a) < 0.0000001) {
			break;}
		if (ts->sincos) {
			ts->sincos[i] = s;
			ts->sincos[i+1] = c;
			//fprintf(stderr, "%i   %i %i\n", i, s, c);
		}
	}
	if (ts->sincos) {
		return 0;
	}
	ts->len = best_i + 2;
	return translate_init(ts);
}

void translate(struct demod_state *d)
{
	int i, len, sc_i, sc_len;
	int32_t tmp, ar, aj, br, bj;
	int16_t *buf = d->lowpassed;
	int16_t *sincos = d->rotate.sincos;
	len = d->lp_len;
	sc_i = d->rotate.i;
	sc_len = d->rotate.len;
	for (i=0; i<len; i+=2, sc_i+=2) {
		sc_i = sc_i % sc_len;
		ar = (int32_t)buf[i];
		aj = (int32_t)buf[i+1];
		br = (int32_t)sincos[sc_i];
		bj = (int32_t)sincos[sc_i+1];
		tmp = ar*br - aj*bj;
		buf[i]   = (int16_t)(tmp >> 14);
		tmp = aj*br + ar*bj;
		buf[i+1]   = (int16_t)(tmp >> 14);
	}
	d->rotate.i = sc_i;
}

void low_pass(struct demod_state *d)
/* simple square window FIR */
{
	int i=0, i2=0;
	while (i < d->lp_len) {
		d->now_r += d->lowpassed[i];
		d->now_j += d->lowpassed[i+1];
		i += 2;
		d->prev_index++;
		if (d->prev_index < d->downsample) {
			continue;
		}
		d->lowpassed[i2]   = d->now_r; // * d->output_scale;
		d->lowpassed[i2+1] = d->now_j; // * d->output_scale;
		d->prev_index = 0;
		d->now_r = 0;
		d->now_j = 0;
		i2 += 2;
	}
	d->lp_len = i2;
}

int low_pass_simple(int16_t *signal2, int len, int step)
// no wrap around, length must be multiple of step
{
	int i, i2, sum;
	for(i=0; i < len; i+=step) {
		sum = 0;
		for(i2=0; i2<step; i2++) {
			sum += (int)signal2[i + i2];
		}
		//signal2[i/step] = (int16_t)(sum / step);
		signal2[i/step] = (int16_t)(sum);
	}
	signal2[i/step + 1] = signal2[i/step];
	return len / step;
}

void low_pass_real(struct demod_state *s)
/* simple square window FIR */
// add support for upsampling?
{
	int16_t *lp = s->lowpassed;
	int i=0, i2=0;
	int fast = (int)s->rate_out;
	int slow = s->rate_out2;
	while (i < s->lp_len) {
		s->now_lpr += lp[i];
		i++;
		s->prev_lpr_index += slow;
		if (s->prev_lpr_index < fast) {
			continue;
		}
		lp[i2] = (int16_t)(s->now_lpr / (fast/slow));
		s->prev_lpr_index -= fast;
		s->now_lpr = 0;
		i2 += 1;
	}
	s->lp_len = i2;
}

void fifth_order(int16_t *data, int length, int16_t *hist)
/* for half of interleaved data */
{
	int i;
	int16_t a, b, c, d, e, f;
	a = hist[1];
	b = hist[2];
	c = hist[3];
	d = hist[4];
	e = hist[5];
	f = data[0];
	/* a downsample should improve resolution, so don't fully shift */
	data[0] = (a + (b+e)*5 + (c+d)*10 + f) >> 4;
	for (i=4; i<length; i+=4) {
		a = c;
		b = d;
		c = e;
		d = f;
		e = data[i-2];
		f = data[i];
		data[i/2] = (a + (b+e)*5 + (c+d)*10 + f) >> 4;
	}
	/* archive */
	hist[0] = a;
	hist[1] = b;
	hist[2] = c;
	hist[3] = d;
	hist[4] = e;
	hist[5] = f;
}

void generic_fir(int16_t *data, int length, int *fir, int16_t *hist)
/* Okay, not at all generic.  Assumes length 9, fix that eventually. */
{
	int d, temp, sum;
	for (d=0; d<length; d+=2) {
		temp = data[d];
		sum = 0;
		sum += (hist[0] + hist[8]) * fir[1];
		sum += (hist[1] + hist[7]) * fir[2];
		sum += (hist[2] + hist[6]) * fir[3];
		sum += (hist[3] + hist[5]) * fir[4];
		sum +=            hist[4]  * fir[5];
		data[d] = sum >> 15 ;
		hist[0] = hist[1];
		hist[1] = hist[2];
		hist[2] = hist[3];
		hist[3] = hist[4];
		hist[4] = hist[5];
		hist[5] = hist[6];
		hist[6] = hist[7];
		hist[7] = hist[8];
		hist[8] = temp;
	}
}

/* define our own complex math ops
   because ARMv5 has no hardware float */

void multiply(int ar, int aj, int br, int bj, int *cr, int *cj)
{
	*cr = ar*br - aj*bj;
	*cj = aj*br + ar*bj;
}

int polar_discriminant(int ar, int aj, int br, int bj)
{
	int cr, cj;
	double angle;
	multiply(ar, aj, br, -bj, &cr, &cj);
	angle = atan2((double)cj, (double)cr);
	return (int)(angle / M_PI * (1<<14));
}

int fast_atan2(int y, int x)
/* pre scaled for int16 */
{
	int yabs, angle;
	int pi4=(1<<12), pi34=3*(1<<12);  // note pi = 1<<14
	if (x==0 && y==0) {
		return 0;
	}
	yabs = y;
	if (yabs < 0) {
		yabs = -yabs;
	}
	if (x >= 0) {
		angle = pi4  - pi4 * (x-yabs) / (x+yabs);
	} else {
		angle = pi34 - pi4 * (x+yabs) / (yabs-x);
	}
	if (y < 0) {
		return -angle;
	}
	return angle;
}

int polar_disc_fast(int ar, int aj, int br, int bj)
{
	int cr, cj;
	multiply(ar, aj, br, -bj, &cr, &cj);
	return fast_atan2(cj, cr);
}

int atan_lut_init(void)
{
	int i = 0;

	atan_lut = malloc(atan_lut_size * sizeof(int));

	for (i = 0; i < atan_lut_size; i++) {
		atan_lut[i] = (int) (atan((double) i / (1<<atan_lut_coef)) / M_PI * (1<<14));
	}

	return 0;
}

int polar_disc_lut(int ar, int aj, int br, int bj)
{
	int cr, cj, x, x_abs;

	multiply(ar, aj, br, -bj, &cr, &cj);

	/* special cases */
	if (cr == 0 || cj == 0) {
		if (cr == 0 && cj == 0)
			{return 0;}
		if (cr == 0 && cj > 0)
			{return 1 << 13;}
		if (cr == 0 && cj < 0)
			{return -(1 << 13);}
		if (cj == 0 && cr > 0)
			{return 0;}
		if (cj == 0 && cr < 0)
			{return 1 << 14;}
	}

	/* real range -32768 - 32768 use 64x range -> absolute maximum: 2097152 */
	x = (cj << atan_lut_coef) / cr;
	x_abs = abs(x);

	if (x_abs >= atan_lut_size) {
		/* we can use linear range, but it is not necessary */
		return (cj > 0) ? 1<<13 : -1<<13;
	}

	if (x > 0) {
		return (cj > 0) ? atan_lut[x] : atan_lut[x] - (1<<14);
	} else {
		return (cj > 0) ? (1<<14) - atan_lut[-x] : -atan_lut[-x];
	}

	return 0;
}

int esbensen(int ar, int aj, int br, int bj)
/*
  input signal: s(t) = a*exp(-i*w*t+p)
  a = amplitude, w = angular freq, p = phase difference
  solve w
  s' = -i(w)*a*exp(-i*w*t+p)
  s'*conj(s) = -i*w*a*a
  s'*conj(s) / |s|^2 = -i*w
*/
{
	int cj, dr, dj;
	int scaled_pi = 2608; /* 1<<14 / (2*pi) */
	dr = (br - ar) * 2;
	dj = (bj - aj) * 2;
	cj = bj*dr - br*dj; /* imag(ds*conj(s)) */
	return (scaled_pi * cj / (ar*ar+aj*aj+1));
}

void fm_demod(struct demod_state *fm)
{
	int i, pcm = 0;
	int16_t *lp = fm->lowpassed;
        int16_t pr = fm->pre_r;
	int16_t pj = fm->pre_j;
	for (i = 0; i < (fm->lp_len-1); i += 2) {
		switch (fm->custom_atan) {
		case 0:
			pcm = polar_discriminant(lp[i], lp[i+1], pr, pj);
			break;
		case 1:
			pcm = polar_disc_fast(lp[i], lp[i+1], pr, pj);
			break;
		case 2:
			pcm = polar_disc_lut(lp[i], lp[i+1], pr, pj);
			break;
		case 3:
			pcm = esbensen(lp[i], lp[i+1], pr, pj);
			break;
		}
		pr = lp[i];
		pj = lp[i+1];
		fm->lowpassed[i/2] = (int16_t)pcm;
	}
	fm->pre_r = pr;
	fm->pre_j = pj;
	fm->lp_len = fm->lp_len / 2;
}

void am_demod(struct demod_state *fm)
// todo, fix this extreme laziness
{
	int32_t i, pcm;
	int16_t *lp = fm->lowpassed;
	for (i = 0; i < fm->lp_len; i += 2) {
		// hypot uses floats but won't overflow
		//r[i/2] = (int16_t)hypot(lp[i], lp[i+1]);
		pcm = lp[i] * lp[i];
		pcm += lp[i+1] * lp[i+1];
		lp[i/2] = (int16_t)sqrt(pcm) * fm->output_scale;
	}
	fm->lp_len = fm->lp_len / 2;
	// lowpass? (3khz)
}

void usb_demod(struct demod_state *fm)
{
	int i, pcm;
	int16_t *lp = fm->lowpassed;
	for (i = 0; i < fm->lp_len; i += 2) {
		pcm = lp[i] + lp[i+1];
		lp[i/2] = (int16_t)pcm * fm->output_scale;
	}
	fm->lp_len = fm->lp_len / 2;
}

void lsb_demod(struct demod_state *fm)
{
	int i, pcm;
	int16_t *lp = fm->lowpassed;
	for (i = 0; i < fm->lp_len; i += 2) {
		pcm = lp[i] - lp[i+1];
		lp[i/2] = (int16_t)pcm * fm->output_scale;
	}
	fm->lp_len = fm->lp_len / 2;
}

void raw_demod(struct demod_state *fm)
{
	return;
}

void deemph_filter(struct demod_state *fm)
{
	static int avg;  // cheating, not threadsafe
	int i, d;
	int16_t *lp = fm->lowpassed;
	// de-emph IIR
	// avg = avg * (1 - alpha) + sample * alpha;
	for (i = 0; i < fm->lp_len; i++) {
		d = lp[i] - avg;
		if (d > 0) {
			avg += (d + fm->deemph_a/2) / fm->deemph_a;
		} else {
			avg += (d - fm->deemph_a/2) / fm->deemph_a;
		}
		lp[i] = (int16_t)avg;
	}
}

void dc_block_filter(struct demod_state *fm)
{
	int i, avg;
	int64_t sum = 0;
	int16_t *lp = fm->lowpassed;
	for (i=0; i < fm->lp_len; i++) {
		sum += lp[i];
	}
	avg = (int)(sum / fm->lp_len);
	avg = (avg + fm->dc_avg * 9) / 10;
	for (i=0; i < fm->lp_len; i++) {
		lp[i] -= avg;
	}
	fm->dc_avg = avg;
}

int mad(int16_t *samples, int len, int step)
/* mean average deviation */
{
	int i=0, sum=0, ave=0;
	if (len == 0)
		{return 0;}
	for (i=0; i<len; i+=step) {
		sum += samples[i];
	}
	ave = sum / (len * step);
	sum = 0;
	for (i=0; i<len; i+=step) {
		sum += abs(samples[i] - ave);
	}
	return sum / (len / step);
}

int rms(int16_t *samples, int len, int step)
/* edits to correctly calculate rms power based on i and q data */
{
	int i;
	long r, j, rms;
	
	rms = 0;
	for (i=0; i<len; i+=2) {
		r = (long)samples[i];
		j = (long)samples[i++];
		rms += sqrt((r*r)+(j*j));
	}
	return rms/len;
}

int squelch_to_rms(int db, struct dongle_state *dongle, struct demod_state *demod)
/* 0 dB = 1 rms at 50dB gain and 1024 downsample */
{
	double linear, gain, downsample;
	if (db == 0) {
		return 0;}
	linear = pow(10.0, (double)db/20.0);
	gain = 50.0;
	if (dongle->gain != AUTO_GAIN) {
		gain = (double)(dongle->gain) / 10.0;
	}
	gain = 50.0 - gain;
	gain = pow(10.0, gain/20.0);
	downsample = 1024.0 / (double)demod->downsample;
	linear = linear / gain;
	linear = linear / downsample;
	return (int)linear + 1;
}

void software_agc(struct demod_state *d)
{
	int i = 0;
	int peaked = 0;
	int32_t output = 0;
	int abs_output = 0;
	struct agc_state *agc = d->agc;
	int16_t *lp = d->lowpassed;
	int attack_step = agc->attack_step;
	int aggressive = agc_aggressive == d->agc_mode;
	float peak_factor = 1.0;

	for (i=0; i < d->lp_len; i++) {
		output = (int32_t)lp[i] * agc->gain_num + agc->error;
		agc->error = output % agc->gain_den;
		output /= agc->gain_den;
		abs_output = abs(output);
		peaked = abs_output > agc->peak_target;
		
		if (peaked && aggressive && attack_step <= 1) {
			peak_factor = fmin(5.0, (float) abs_output / agc->peak_target);
			attack_step = (int) (pow(agc->attack_step - peak_factor, peak_factor) * (176 + 3 * peak_factor));
		}

		if (peaked) {
			agc->gain_num -= attack_step;
			if (aggressive) { 
				attack_step = (int) (attack_step / 1.2); 
			}
		} else {
			agc->gain_num += agc->decay_step;
		}

		if (agc->gain_num < agc->gain_den) {
			agc->gain_num = agc->gain_den;}
		if (agc->gain_num > agc->gain_max) {
			agc->gain_num = agc->gain_max;}

		if (output >= (1<<15)) {
			output = (1<<15) - 1;}
		if (output < -(1<<15)) {
			output = -(1<<15) + 1;}

		lp[i] = (int16_t)output;
	}
}

void full_demod(struct demod_state *d)
{
	double freqK, avgRms, rmsLevel, avgRmsLevel;
	int i, ds_p;
	int do_squelch = 0;
	int sr = 0;
	if(d->rotate_enable) {
		translate(d);
	}
	ds_p = d->downsample_passes;
	if (ds_p) {
		for (i=0; i < ds_p; i++) {
			fifth_order(d->lowpassed,   (d->lp_len >> i), d->lp_i_hist[i]);
			fifth_order(d->lowpassed+1, (d->lp_len >> i) - 1, d->lp_q_hist[i]);
		}
		d->lp_len = d->lp_len >> ds_p;
		/* droop compensation */
		if (d->comp_fir_size == 9 && ds_p <= CIC_TABLE_MAX) {
			generic_fir(d->lowpassed, d->lp_len,
				cic_9_tables[ds_p], d->droop_i_hist);
			generic_fir(d->lowpassed+1, d->lp_len-1,
				cic_9_tables[ds_p], d->droop_q_hist);
		}
	} else {
		low_pass(d);
	}
	/* power squelch */
	if (d->squelch_level) {
		sr = rms(d->lowpassed, d->lp_len, 1);
		if (d->log3) {
			fprintf(stderr, "freq,%10u,Hz,-,signal,%5u,%5u,%5u,%5u,%5u\r", freqd, sr, d->squelch_level, d->squelch_hits, d->conseq_squelch, d->squelch_timer);
		}
		if (d->log4) {
			fprintf(stderr, "freq,%10u,Hz,-,signal,%5u,%5u\n", freqd, sr, d->squelch_level);
		}		
		if (sr < d->squelch_level) {
			do_squelch = 1;}
	}
	if (do_squelch) {
		d->squelch_hits++;
		d->squelch_timer = 0;
		if (d->log2) {
			if (d->squelch_hits > 0) {
				time_t long_time;
				struct timeval tv;
				struct tm *newtime;
				gettimeofday(&tv,0);
				time(&long_time);
				newtime = localtime(&long_time);
				fprintf(stderr, "freq,%10u,Hz,-,signal,%5u,%5u,-,%04d.%02d.%02d,time,%02d:%02d:%02d.%03ld\n", freqd, sr, d->squelch_level, newtime->tm_year+1900, newtime->tm_mon+1, newtime->tm_mday, newtime->tm_hour,newtime->tm_min,newtime->tm_sec, (long)tv.tv_usec / 1000);
			}
		}		
		for (i=0; i<d->lp_len; i++) {
			d->lowpassed[i] = 0;
		}
		if (d->ds1) {
			d->squelch_level = sr * 1.8;
			fprintf(stderr, "ds1,%u,%d\n", sr, d->squelch_level);
		}		
	} else {
		if (d->log1) {
			if (d->squelch_hits > 0) {
				time_t long_time;
				struct timeval tv;
				struct tm *newtime;
				gettimeofday(&tv,0);
				time(&long_time);
				newtime = localtime(&long_time);
				fprintf(stderr, "%u,%u,%u,%u,%u,%04d.%02d.%02d,%02d:%02d:%02d.%03ld,SO\n", freqd, sr, d->squelch_level, d->conseq_squelch, d->squelch_timer, newtime->tm_year+1900, newtime->tm_mon+1, newtime->tm_mday, newtime->tm_hour,newtime->tm_min,newtime->tm_sec, (long)tv.tv_usec / 1000);
			}
		}		
		d->squelch_hits = 0;
		d->squelch_timer++;
	}
	if (d->squelch_level && d->squelch_hits > d->conseq_squelch) {
		d->agc->gain_num = d->agc->gain_den;
	}

	if (printLevels) {
		if (!sr)
			sr = rms(d->lowpassed, d->lp_len, 1);
		--printLevelNo;
		if (printLevels && sr >= 0) {
			levelSum += sr;
			if (levelMax < sr)		levelMax = sr;
			if (levelMaxMax < sr)	levelMaxMax = sr;
			if  (!printLevelNo) {
				printLevelNo = printLevels;
				freqK = dongle.userFreq /1000.0;
				avgRms = levelSum / printLevels;
				rmsLevel = 20.0 * log10( 1E-10 + sr );
				avgRmsLevel = 20.0 * log10( 1E-10 + avgRms );
				fprintf(stderr, "%.3f kHz, %.1f avg rms, %d max rms, %d max max rms, %d squelch rms, %d rms, %.1f dB rms level, %.2f dB avg rms level\n",
					freqK, avgRms, levelMax, levelMaxMax, d->squelch_level, sr, rmsLevel, avgRmsLevel );
				levelMax = 0;
				levelSum = 0;
			}
		}
	}

	d->mode_demod(d);  /* lowpassed -> result */
	if (d->mode_demod == &raw_demod) {
		return;}
	if (d->dc_block) {
		dc_block_filter(d);}
	if (d->agc_mode != agc_off) {
		software_agc(d);}
	/* todo, fm noise squelch */
	// use nicer filter here too?
	if (d->post_downsample > 1) {
		d->lp_len = low_pass_simple(d->lowpassed, d->lp_len, d->post_downsample);}
	if (d->deemph) {
		deemph_filter(d);}
	if (d->rate_out2 > 0) {
		low_pass_real(d);
		//arbitrary_resample(d->lowpassed, d->lowpassed, d->lp_len, d->lp_len * d->rate_out2 / d->rate_out);
	}
}

int16_t* mark_shared_buffer(void)
{
	int i = 0;
	for (i=0; i<SHARED_SIZE; i++) {
		if (ss_busy[i] == 0) {
			ss_busy[i] = 1;
			return shared_samples[i];
		}
	}
	/* worst case, nuke a buffer */
	ss_busy[0];
	return shared_samples[0];
}

int unmark_shared_buffer(int16_t *buf)
{
	int i;
	for (i=0; i<SHARED_SIZE; i++) {
		if (shared_samples[i] == buf) {
			ss_busy[i] = 0;
			return 0;
		}
	}
	return 1;
}

static void rtlsdr_callback(unsigned char *buf, uint32_t len, void *ctx)
{
	int i;
	struct dongle_state *s = ctx;
	struct demod_state *d;
	struct demod_state *d2;

	if (do_exit) {
		return;}
	if (!s) {
		return;}
	d  = s->targets[0];
	d2 = s->targets[1];
	if (s->mute) {
		for (i=0; i<s->mute; i++) {
			buf[i] = 127;}
		s->mute = 0;
	}
	if (s->pre_rotate) {
		rotate_90(buf, len);}
	for (i=0; i<(int)len; i++) {
		s->buf16[i] = (int16_t)buf[i] - 127;}
	if (d2 != NULL) {
		pthread_rwlock_wrlock(&d2->rw);
		d2->lowpassed = mark_shared_buffer();
		memcpy(d2->lowpassed, s->buf16, 2*len);
		d2->lp_len = len;
		pthread_rwlock_unlock(&d2->rw);
		safe_cond_signal(&d2->ready, &d2->ready_m);
	}
	pthread_rwlock_wrlock(&d->rw);
	d->lowpassed = s->buf16;
	d->lp_len = len;
	pthread_rwlock_unlock(&d->rw);
	safe_cond_signal(&d->ready, &d->ready_m);
	s->buf16 = mark_shared_buffer();
}

static void *dongle_thread_fn(void *arg)
{
	struct dongle_state *s = arg;
	rtlsdr_read_async(s->dev, rtlsdr_callback, s, 0, s->buf_len);
	return 0;
}

static void *demod_thread_fn(void *arg)
{
	struct demod_state *d = arg;
	struct buffer_bucket *o = d->output_target;
	while (!do_exit) {
		safe_cond_wait(&d->ready, &d->ready_m);
		pthread_rwlock_wrlock(&d->rw);
		full_demod(d);
		pthread_rwlock_unlock(&d->rw);
		if (d->exit_flag) {
			do_exit = 1;
		}
		pthread_rwlock_wrlock(&o->rw);
		o->buf = d->lowpassed;
		o->len = d->lp_len;
		pthread_rwlock_unlock(&o->rw);
		if (controller.freq_len > 1 && d->squelch_level) {
		    if (d->squelch_hits > d->conseq_squelch) {
			unmark_shared_buffer(d->lowpassed);
			d->squelch_hits = d->conseq_squelch + 1;  /* hair trigger */
			d->squelch_timer = 0;
			safe_cond_signal(&controller.hop, &controller.hop_m);
			continue;
		    }

		    if (d->squelch_timer >= d->squelch_timeout) {
			    d->squelch_timer = 0;
			    safe_cond_signal(&controller.hop, &controller.hop_m);
			    continue;
		    }
		}
		safe_cond_signal(&o->ready, &o->ready_m);
		pthread_mutex_lock(&o->trycond_m);
		o->trycond = 0;
		pthread_mutex_unlock(&o->trycond_m);
	}
	return 0;
}

#ifndef _WIN32
static int get_nanotime(struct timespec *ts)
{
	int rv = ENOSYS;
#ifdef __unix__
	rv = clock_gettime(CLOCK_MONOTONIC, ts);
#elif __APPLE__
	struct timeval tv;

	rv = gettimeofday(&tv, NULL);
	ts->tv_sec = tv.tv_sec;
	ts->tv_nsec = tv.tv_usec * 1000L;
#endif
	return rv;
}
#endif

static void *output_thread_fn(void *arg)
{
	int j, r = 0;
	struct output_state *s = arg;
	struct buffer_bucket *b0 = &s->results[0];
	struct buffer_bucket *b1 = &s->results[1];
	int64_t i, duration, samples = 0LL, samples_now;
#ifdef _WIN32
	LARGE_INTEGER perfFrequency;
	LARGE_INTEGER start_time;
	LARGE_INTEGER now_time;

	QueryPerformanceFrequency(&perfFrequency);
	QueryPerformanceCounter(&start_time);
#else
	struct timespec start_time;
	struct timespec now_time;

	get_nanotime(&start_time);
#endif
	while (!do_exit) {
		if (s->lrmix) {
			safe_cond_wait(&b0->ready, &b0->ready_m);
			pthread_rwlock_rdlock(&b0->rw);
			safe_cond_wait(&b1->ready, &b1->ready_m);
			pthread_rwlock_rdlock(&b1->rw);
			for(j=0; j < b0->len; j++) {
				fwrite(b0->buf+j, 2, 1, s->file);
				fwrite(b1->buf+j, 2, 1, s->file);
			}
			unmark_shared_buffer(b1->buf);
			pthread_rwlock_unlock(&b1->rw);
			unmark_shared_buffer(b0->buf);
			pthread_rwlock_unlock(&b0->rw);
			continue;
		}
		if (!s->padded) {
			safe_cond_wait(&b0->ready, &b0->ready_m);
			pthread_rwlock_rdlock(&b0->rw);
			fwrite(b0->buf, 2, b0->len, s->file);
			unmark_shared_buffer(b0->buf);
			pthread_rwlock_unlock(&b0->rw);
			continue;
		}

		/* padding requires output at constant rate */
		/* pthread_cond_timedwait is terrible, roll our own trycond */
		usleep(2000);
		pthread_mutex_lock(&b0->trycond_m);
		r = b0->trycond;
		b0->trycond = 1;
		pthread_mutex_unlock(&b0->trycond_m);
		if (r == 0) {
			pthread_rwlock_rdlock(&b0->rw);
			fwrite(b0->buf, 2, b0->len, s->file);
			unmark_shared_buffer(b0->buf);
			samples += (int64_t)b0->len;
			pthread_rwlock_unlock(&b0->rw);
			continue;
		}
#ifdef _WIN32
		QueryPerformanceCounter(&now_time);
		duration = now_time.QuadPart - start_time.QuadPart;
		samples_now = (duration * s->rate) / perfFrequency.QuadPart;
#else
		get_nanotime(&now_time);
		duration = now_time.tv_sec - start_time.tv_sec;
		duration *= 1000000000L;
		duration += (now_time.tv_nsec - start_time.tv_nsec);
		samples_now = (duration * s->rate) / 1000000000UL;
#endif
		if (samples_now < samples) {
			continue;}
		for (i=samples; i<samples_now; i++) {
			fputc(0, s->file);
			fputc(0, s->file);
		}
		samples = samples_now;
	}
	return 0;
}

static void optimal_settings(int freq, int rate)
{
	// giant ball of hacks
	// seems unable to do a single pass, 2:1
	int capture_freq, capture_rate;
	struct dongle_state *d = &dongle;
	struct demod_state *dm = &demod;
	struct controller_state *cs = &controller;
	dm->downsample = (MINIMUM_RATE / dm->rate_in) + 1;
	if (dm->downsample_passes) {
		dm->downsample_passes = (int)log2(dm->downsample) + 1;
		dm->downsample = 1 << dm->downsample_passes;
	}
	if (verbosity >= 2) {
		fprintf(stderr, "downsample_passes = %d (= # of fifth_order() iterations), downsample = %d\n", dm->downsample_passes, dm->downsample );
	}
	capture_freq = freq;
	capture_rate = dm->downsample * dm->rate_in;
	if (d->pre_rotate) {
		capture_freq = freq + capture_rate/4;}
	if (verbosity >= 2)
		fprintf(stderr, "capture_rate = dm->downsample * dm->rate_in = %d * %d = %d\n", dm->downsample, dm->rate_in, capture_rate );
	capture_freq += cs->edge * dm->rate_in / 2;
	if (verbosity >= 2)
		fprintf(stderr, "optimal_settings(freq = %u): capture_freq +=  cs->edge * dm->rate_in / 2 = %d * %d / 2 = %u\n", freq, cs->edge, dm->rate_in, capture_freq );
	dm->output_scale = (1<<15) / (128 * dm->downsample);
	if (dm->output_scale < 1) {
		dm->output_scale = 1;}
	if (dm->mode_demod == &fm_demod) {
		dm->output_scale = 1;}
	d->userFreq = freq;
	d->freq = (uint32_t)capture_freq;
	d->rate = (uint32_t)capture_rate;
	//d->pre_rotate = 0;
	//demod.rotate_enable = 1;
	//demod.rotate.angle = -0.25 * 2 * M_PI;
	//translate_init(&demod.rotate);
	if (verbosity >= 2)
		fprintf(stderr, "optimal_settings(freq = %u) delivers freq %.0f, rate %.0f\n", freq, (double)d->freq, (double)d->rate );
}

void clone_demod(struct demod_state *d2, struct demod_state *d1)
/* copy from d1 to d2 */
{
	d2->rate_in = d1->rate_in;
	d2->rate_out = d1->rate_out;
	d2->rate_out2 = d1->rate_out2;
	d2->downsample = d1->downsample;
	d2->downsample_passes = d1->downsample_passes;
	d2->post_downsample = d1->post_downsample;
	d2->output_scale = d1->output_scale;
	d2->squelch_level = d1->squelch_level;
	d2->conseq_squelch = d1->conseq_squelch;
	d2->squelch_hits = d1->squelch_hits;
	d2->terminate_on_squelch = d1->terminate_on_squelch;
	d2->comp_fir_size = d1->comp_fir_size;
	d2->custom_atan = d1->custom_atan;
	d2->deemph = d1->deemph;
	d2->deemph_a = d1->deemph_a;
	d2->dc_block = d1->dc_block;
	d2->rotate_enable = d1->rotate_enable;
	d2->agc_mode = d1->agc_mode;
	d2->mode_demod = d1->mode_demod;
}

void optimal_lrmix(void)
{
	double angle1, angle2;
	uint32_t freq, freq1, freq2, bw, dongle_bw, mr;
	if (controller.freq_len != 2) {
		fprintf(stderr, "error: lrmix requires two frequencies\n");
		do_exit = 1;
		exit(1);
	}
	if (output.padded) {
		fprintf(stderr, "warning: lrmix does not support padding\n");
	}
	freq1 = controller.freqs[0].freq;
	freq2 = controller.freqs[1].freq;
	bw = demod.rate_out;
	freq = freq1 / 2 + freq2 / 2 + bw;
	mr = (uint32_t)abs((int64_t)freq1 - (int64_t)freq2) + bw;
	if (mr > MINIMUM_RATE) {
		MINIMUM_RATE = mr;}
	dongle.pre_rotate = 0;
	optimal_settings(freq, bw);
	output.padded = 0;
	clone_demod(&demod2, &demod);
	//demod2 = demod;
	demod2.output_target = &output.results[1];
	dongle.targets[1] = &demod2;
	dongle_bw = dongle.rate;
	if (dongle_bw > MAXIMUM_RATE) {
		fprintf(stderr, "error: unable to find optimal settings\n");
		do_exit = 1;
		exit(1);
	}
	angle1 = ((double)freq1 - (double)freq) / (double)dongle_bw;
	demod.rotate.angle = angle1 * 2 * M_PI;
	angle2 = ((double)freq2 - (double)freq) / (double)dongle_bw;
	demod2.rotate.angle = angle2 * 2 * M_PI;
	translate_init(&demod.rotate);
	translate_init(&demod2.rotate);
	//fprintf(stderr, "a1 %f, a2 %f\n", angle1, angle2);
}

static void *controller_thread_fn(void *arg)
{
	// thoughts for multiple dongles
	// might be no good using a controller thread if retune/rate blocks
	int i;
	struct controller_state *s = arg;
	double elapsedtime;
	freq_state *f;

	if (s->freq_len > 1) {
		demod.squelch_level = s->freqs[0].squelch;
		demod.conseq_squelch = s->freqs[0].conseq_squelch;
		demod.squelch_timeout = s->freqs[0].squelch_timeout;
	}

	if (s->wb_mode) {
		if (verbosity)
			fprintf(stderr, "wbfm: adding 16000 Hz to every input frequency\n");
		for (i=0; i < s->freq_len; i++) {
			s->freqs[i].freq += 16000;}
	}

	/* set up primary channel */
	optimal_settings(s->freqs[0].freq, demod.rate_in);
	demod.squelch_level = squelch_to_rms(demod.squelch_level, &dongle, &demod);
	if (dongle.direct_sampling) {
		verbose_direct_sampling(dongle.dev, dongle.direct_sampling);}
	if (dongle.offset_tuning) {
		verbose_offset_tuning(dongle.dev);}

	/* set up lrmix */
	if (output.lrmix) {
		optimal_lrmix();
	}

	/* Set the frequency */
	if (verbosity) {
		fprintf(stderr, "verbose_set_frequency(%.3f kHz)\n", (double)dongle.userFreq /1000.0);
		fprintf(stderr, "  frequency is away from parametrized one, to avoid negative impact from dc\n");
	}
	freqd = dongle.freq;
	verbose_set_frequency(dongle.dev, dongle.freq);
	fprintf(stderr, "Oversampling input by: %ix.\n", demod.downsample);
	fprintf(stderr, "Oversampling output by: %ix.\n", demod.post_downsample);
	fprintf(stderr, "Buffer size: %0.2fms\n",
		1000 * 0.5 * (float)ACTUAL_BUF_LENGTH / (float)dongle.rate);

	/* Set the sample rate */
	if (verbosity)
		fprintf(stderr, "verbose_set_sample_rate(%.0f Hz)\n", (double)dongle.rate);
	verbose_set_sample_rate(dongle.dev, dongle.rate);
	fprintf(stderr, "Output at %u Hz.\n", demod.rate_in/demod.post_downsample);

	while (!do_exit) {
		safe_cond_wait(&s->hop, &s->hop_m);
		if (s->freq_len <= 1) {
			continue;}
		if (output.lrmix) {
			continue;}
		/* hacky hopping */
		s->freq_now = (s->freq_now + 1) % s->freq_len;
		s->freq = &s->freqs[s->freq_now];
		f = s->freq;		
		if (s->freq_now == 0) {
			if (controller.speedtest) {
				gettimeofday(&time2, NULL);
				elapsedtime = (time2.tv_sec - time1.tv_sec) * 1000.0;
				elapsedtime += (time2.tv_usec - time1.tv_usec) / 1000.0;
				fprintf(stderr, "%8.2f steps/s - elapsed time %6.0f ms -  %5.0u steps\n",1/(elapsedtime/s->freq_len)*1000 ,elapsedtime, s->freq_len);
				time1 = time2;}
			/* single-shot mode */
			if (single) {
				do_exit = 1;}
		}
		freqd = f->freq;
		if (s->wb_mode) {
		    freqd -= 16000;}
		
		demod.squelch_level = f->squelch;
		if (demod.squelch_hits == demod.conseq_squelch+1) {
			demod.squelch_hits = f->conseq_squelch + 1;
		}
		demod.conseq_squelch = f->conseq_squelch;
		demod.squelch_timeout = f->squelch_timeout;
		demod.squelch_level = squelch_to_rms(demod.squelch_level, &dongle, &demod);

		optimal_settings(f->freq, demod.rate_in);
		rtlsdr_set_center_freq(dongle.dev, dongle.freq);
		dongle.mute = BUFFER_DUMP;
	}
	return 0;
}

void frequency_range(struct controller_state *s, char *arg)
{
	char *start, *stop, *step;
	int i;
	freq_state *f;
	start = arg;
	stop = strchr(start, ':') + 1;
	stop[-1] = '\0';
	step = strchr(stop, ':') + 1;
	step[-1] = '\0';
	for(i=(int)atofs(start); i<=(int)atofs(stop); i+=(int)atofs(step))
	{
		f = &s->freqs[s->freq_len];
		f->freq = (uint32_t)i;
		f->squelch = demod.squelch_level;
		f->conseq_squelch = demod.conseq_squelch;
		s->freq_len++;
		if (s->freq_len >= FREQUENCIES_LIMIT) {
			break;}
	}
	stop[-1] = ':';
	step[-1] = ':';
}

void dongle_init(struct dongle_state *s)
{
	s->rate = DEFAULT_SAMPLE_RATE;
	s->gain = AUTO_GAIN; // tenths of a dB
	s->mute = 0;
	s->direct_sampling = 0;
	s->offset_tuning = 0;
	s->pre_rotate = 1;
	s->targets[0] = &demod;
	s->targets[1] = NULL;
	s->buf16 = mark_shared_buffer();
	s->bandwidth = 0;
}

void demod_init(struct demod_state *s)
{
	s->rate_in = DEFAULT_SAMPLE_RATE;
	s->rate_out = DEFAULT_SAMPLE_RATE;
	s->squelch_level = 0;
	s->conseq_squelch = 10;
	s->terminate_on_squelch = 0;
	s->squelch_hits = 11;
	s->squelch_timer = 0;
	s->squelch_timeout = SQUELCH_TIMEOUT;
	s->downsample_passes = 0;
	s->comp_fir_size = 0;
	s->prev_index = 0;
	s->post_downsample = 1;  // once this works, default = 4
	s->custom_atan = 0;
	s->deemph = 0;
	s->agc_mode = agc_off;
	s->rotate_enable = 0;
	s->rotate.len = 0;
	s->rotate.sincos = NULL;
	s->rate_out2 = -1;  // flag for disabled
	s->mode_demod = &fm_demod;
	s->pre_j = s->pre_r = s->now_r = s->now_j = 0;
	s->prev_lpr_index = 0;
	s->deemph_a = 0;
	s->now_lpr = 0;
	s->dc_block = 1;
	s->dc_avg = 0;
	s->log1 = 0;
	s->log2 = 0;
	s->log3 = 0;
	s->log4 = 0;
	s->ds1 = 0;	
	pthread_rwlock_init(&s->rw, NULL);
	pthread_cond_init(&s->ready, NULL);
	pthread_mutex_init(&s->ready_m, NULL);
	s->output_target = &output.results[0];
	s->lowpassed = NULL;
}

void demod_cleanup(struct demod_state *s)
{
	pthread_rwlock_destroy(&s->rw);
	pthread_cond_destroy(&s->ready);
	pthread_mutex_destroy(&s->ready_m);
}

void output_init(struct output_state *s)
{
	int i;
	//s->rate = DEFAULT_SAMPLE_RATE;
	for (i=0; i<2; i++) {
		pthread_rwlock_init(&s->results[i].rw, NULL);
		pthread_cond_init(&s->results[i].ready, NULL);
		pthread_mutex_init(&s->results[i].ready_m, NULL);
		pthread_mutex_init(&s->results[i].trycond_m, NULL);
		s->results[i].trycond = 1;
		s->results[i].buf = NULL;
	}
}

void output_cleanup(struct output_state *s)
{
	int i;
	for (i=0; i<2; i++) {
		pthread_rwlock_destroy(&s->results[i].rw);
		pthread_cond_destroy(&s->results[i].ready);
		pthread_mutex_destroy(&s->results[i].ready_m);
		pthread_mutex_destroy(&s->results[i].trycond_m);
	}
}

void controller_init(struct controller_state *s)
{
	s->freqs[0].freq = 100000000;
	s->freq_len = 0;
	s->edge = 0;
	s->speedtest = 0;
	s->wb_mode = 0;
	pthread_cond_init(&s->hop, NULL);
	pthread_mutex_init(&s->hop_m, NULL);
}

void controller_cleanup(struct controller_state *s)
{
	pthread_cond_destroy(&s->hop);
	pthread_mutex_destroy(&s->hop_m);
}

void sanity_checks(void)
{
	if (controller.freq_len == 0) {
		fprintf(stderr, "Please specify a frequency.\n");
		exit(1);
	}

	if (controller.freq_len >= FREQUENCIES_LIMIT) {
		fprintf(stderr, "Too many channels, maximum %i.\n", FREQUENCIES_LIMIT);
		exit(1);
	}

	if (!output.lrmix && controller.freq_len > 1 && demod.squelch_level == 0) {
		fprintf(stderr, "Please specify a squelch level.  Required for scanning multiple frequencies.\n");
		exit(1);
	}

	if (demod.mode_demod == &raw_demod && output.lrmix) {
		fprintf(stderr, "'raw' is not a supported demodulator for lrmix\n");
		exit(1);
	}

}

int agc_init(struct demod_state *s)
{
	struct agc_state *agc;

	agc = malloc(sizeof(struct agc_state));
	s->agc = agc;

	agc->gain_den = 1<<15;
	agc->peak_target = 1<<14;
	agc->gain_max = 256 * agc->gain_den;
	agc->gain_num = agc->gain_den;
	agc->decay_step = 1; 
	agc->attack_step = 2; 
	if (s->agc_mode == agc_aggressive) {
		agc->decay_step = agc->decay_step * 4; 
		agc->attack_step = agc->attack_step * 5; 
	}

	return 0;
}

int generate_header(struct demod_state *d, struct output_state *o)
{
	int i, s_rate, b_rate;
	char *channels = "\1\0";
	char *align = "\2\0";
	uint8_t samp_rate[4] = {0, 0, 0, 0};
	uint8_t byte_rate[4] = {0, 0, 0, 0};
	s_rate = o->rate;
	b_rate = o->rate * 2;
	if (d->mode_demod == &raw_demod || o->lrmix) {
		channels = "\2\0";
		align = "\4\0";
		b_rate *= 2;
	}
	for (i=0; i<4; i++) {
		samp_rate[i] = (uint8_t)((s_rate >> (8*i)) & 0xFF);
		byte_rate[i] = (uint8_t)((b_rate >> (8*i)) & 0xFF);
	}
	fwrite("RIFF",     1, 4, o->file);
	fwrite("\xFF\xFF\xFF\xFF", 1, 4, o->file);  /* size */
	fwrite("WAVE",     1, 4, o->file);
	fwrite("fmt ",     1, 4, o->file);
	fwrite("\x10\0\0\0", 1, 4, o->file);  /* size */
	fwrite("\1\0",     1, 2, o->file);  /* pcm */
	fwrite(channels,   1, 2, o->file);
	fwrite(samp_rate,  1, 4, o->file);
	fwrite(byte_rate,  1, 4, o->file);
	fwrite(align, 1, 2, o->file);
	fwrite("\x10\0",     1, 2, o->file);  /* bits per channel */
	fwrite("data",     1, 4, o->file);
	fwrite("\xFF\xFF\xFF\xFF", 1, 4, o->file);  /* size */
	return 0;
}

int main(int argc, char **argv)
{
#ifndef _WIN32
	struct sigaction sigact;
#endif
	int r, opt;
	int dev_given = 0;
	int custom_ppm = 0;
	int freqsfile = 0;
	int i;

	int timeConstant = 75; /* default: U.S. 75 uS */
	int rtlagc = 0;
	dongle_init(&dongle);
	demod_init(&demod);
	demod_init(&demod2);
	output_init(&output);
	controller_init(&controller);

	while ((opt = getopt(argc, argv, "d:f:m:g:s:b:l:L:o:t:r:p:E:q:U:F:A:M:c:v:h:w:1:")) != -1) {
		switch (opt) {
		case 'd':
			dongle.dev_index = verbose_device_search(optarg);
			dev_given = 1;
			break;
		case 'f':
			if (controller.freq_len >= FREQUENCIES_LIMIT) {
				break;}
			if (strchr(optarg, ':'))
				{frequency_range(&controller, optarg);}
			else
			{
				freq_state *f = &controller.freqs[controller.freq_len];
				f->freq = (uint32_t)atofs(optarg);				
				controller.freq_len++;
			}
			break;
		case 'm':
			MINIMUM_RATE = (int)atofs(optarg);
			break;
		case 'g':
			dongle.gain = (int)(atof(optarg) * 10);
			break;
		case 'l':
			demod.squelch_level = (int)atof(optarg);
			break;
		case 'L':
			printLevels = (int)atof(optarg);
			break;
		case 's':
			demod.rate_in = (uint32_t)atofs(optarg);
			demod.rate_out = (uint32_t)atofs(optarg);
			break;
		case 'r':
			output.rate = (int)atofs(optarg);
			demod.rate_out2 = (int)atofs(optarg);
			break;
		case 'o':
			fprintf(stderr, "Warning: -o is very buggy\n");
			demod.post_downsample = (int)atof(optarg);
			if (demod.post_downsample < 1 || demod.post_downsample > MAXIMUM_OVERSAMPLE) {
				fprintf(stderr, "Oversample must be between 1 and %i\n", MAXIMUM_OVERSAMPLE);}
			break;
		case 't':
			demod.conseq_squelch = (int)atof(optarg);
			if (demod.conseq_squelch < 0) {
				demod.conseq_squelch = -demod.conseq_squelch;
				demod.terminate_on_squelch = 1;
			}
			break;
		case 'p':
			dongle.ppm_error = atoi(optarg);
			custom_ppm = 1;
			break;
		case 'U':
			output.freqsfilename = optarg;
			freqsfile = 1;
			break;			
		case '1':
			single = 1;
			break;			
		case 'E':
			if (strcmp("edge",  optarg) == 0) {
				controller.edge = 1;}
			if (strcmp("no-dc", optarg) == 0) {
				demod.dc_block = 0;}
			if (strcmp("deemp",  optarg) == 0) {
				demod.deemph = 1;}
			if (strcmp("swagc",  optarg) == 0) {
				demod.agc_mode = agc_normal;}
			if (strcmp("swagc-aggressive",  optarg) == 0) {
				demod.agc_mode = agc_aggressive;}
			if (strcmp("direct",  optarg) == 0) {
				dongle.direct_sampling = 1;}
			if (strcmp("no-mod",  optarg) == 0) {
				dongle.direct_sampling = 3;}
			if (strcmp("offset",  optarg) == 0) {
				dongle.offset_tuning = 1;
				dongle.pre_rotate = 0;}
			if (strcmp("log1", optarg) == 0) {
				demod.log1 = 1;}
			if (strcmp("log2", optarg) == 0) {
				demod.log2 = 1;}
			if (strcmp("log3", optarg) == 0) {
				demod.log3 = 1;}
			if (strcmp("log4", optarg) == 0) {
				demod.log4 = 1;}			
			if (strcmp("speedtest",  optarg) == 0) {
				controller.speedtest = 1;}
			if (strcmp("ds1", optarg) == 0) {
				demod.ds1 = 1;}			
			if (strcmp("wav",  optarg) == 0) {
				output.wav_format = 1;}
			if (strcmp("pad",  optarg) == 0) {
				output.padded = 1;}
			if (strcmp("lrmix",  optarg) == 0) {
				output.lrmix = 1;}
			if (strcmp("rtlagc", optarg) == 0 || strcmp("agc", optarg) == 0) {
				rtlagc = 1;}
			break;
		case 'F':
			demod.downsample_passes = 1;  /* truthy placeholder */
			demod.comp_fir_size = atoi(optarg);
			break;
		case 'A':
			if (strcmp("std",  optarg) == 0) {
				demod.custom_atan = 0;}
			if (strcmp("fast", optarg) == 0) {
				demod.custom_atan = 1;}
			if (strcmp("lut",  optarg) == 0) {
				atan_lut_init();
				demod.custom_atan = 2;}
			if (strcmp("ale", optarg) == 0) {
				demod.custom_atan = 3;}
			break;
		case 'M':
			if (strcmp("nbfm",  optarg) == 0 || strcmp("nfm",  optarg) == 0 || strcmp("fm",  optarg) == 0) {
				demod.mode_demod = &fm_demod;}
			if (strcmp("raw",  optarg) == 0 || strcmp("iq",  optarg) == 0) {
				demod.mode_demod = &raw_demod;}
			if (strcmp("am",  optarg) == 0) {
				demod.mode_demod = &am_demod;}
			if (strcmp("usb", optarg) == 0) {
				demod.mode_demod = &usb_demod;}
			if (strcmp("lsb", optarg) == 0) {
				demod.mode_demod = &lsb_demod;}
			if (strcmp("wbfm",  optarg) == 0 || strcmp("wfm",  optarg) == 0) {
				controller.wb_mode = 1;
				demod.mode_demod = &fm_demod;
				demod.rate_in = 170000;
				demod.rate_out = 170000;
				demod.rate_out2 = 32000;
				output.rate = 32000;
				demod.custom_atan = 1;
				//demod.post_downsample = 4;
				demod.deemph = 1;
				demod.squelch_level = 0;}
			break;
		case 'c':
			if (strcmp("us",  optarg) == 0)
				timeConstant = 75;
			else if (strcmp("eu", optarg) == 0)
				timeConstant = 50;
			else
				timeConstant = (int)atof(optarg);
			break;
		case 'v':
			++verbosity;
			break;
    case 'w':
      dongle.bandwidth = (uint32_t)atofs(optarg);
      break;
		case 'h':
		case '?':
		default:
			usage();
			break;
		}
	}

	if (verbosity)
		fprintf(stderr, "verbosity set to %d\n", verbosity);


	for (i = 0; i < FREQUENCIES_LIMIT; i++) {
		freq_state *f = &controller.freqs[i];
		f->squelch = demod.squelch_level;
		f->conseq_squelch = demod.conseq_squelch;
		f->gain = dongle.gain;
		f->ppm = dongle.ppm_error;		
	}
	

	agc_init(&demod);

	if (freqsfile == 1) {
		load_frequencies(output.freqsfilename, 0);
	} else {
		output.freqsfilename = NULL;
	}

	/* quadruple sample_rate to limit to Δθ to ±π/2 */
	demod.rate_in *= demod.post_downsample;

	if (!output.rate) {
		output.rate = demod.rate_out;}

	sanity_checks();

	if (controller.freq_len > 1) {
		demod.terminate_on_squelch = 0;}

	if (argc <= optind) {
		output.filename = "-";
	} else {
		output.filename = argv[optind];
	}

	ACTUAL_BUF_LENGTH = lcm_post[demod.post_downsample] * DEFAULT_BUF_LENGTH;

	if (!dev_given) {
		dongle.dev_index = verbose_device_search("0");
	}

	if (dongle.dev_index < 0) {
		exit(1);
	}

	r = rtlsdr_open(&dongle.dev, (uint32_t)dongle.dev_index);
	if (r < 0) {
		fprintf(stderr, "Failed to open rtlsdr device #%d.\n", dongle.dev_index);
		exit(1);
	}
#ifndef _WIN32
	sigact.sa_handler = sighandler;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_flags = 0;
	sigaction(SIGINT, &sigact, NULL);
	sigaction(SIGTERM, &sigact, NULL);
	sigaction(SIGQUIT, &sigact, NULL);
	sigaction(SIGPIPE, &sigact, NULL);
	sigaction(SIGUSR1, &sigact, NULL);	
	signal(SIGPIPE, SIG_IGN);
#else
	SetConsoleCtrlHandler( (PHANDLER_ROUTINE) sighandler, TRUE );
	output.padded = 0;
#endif

	if (demod.deemph) {
		double tc = (double)timeConstant * 1e-6;
		demod.deemph_a = (int)round(1.0/((1.0-exp(-1.0/(demod.rate_out * tc)))));
		if (verbosity)
			fprintf(stderr, "using wbfm deemphasis filter with time constant %d us\n", timeConstant );
	}

	/* Set the tuner gain */
	if (dongle.gain == AUTO_GAIN) {
		verbose_auto_gain(dongle.dev);
	} else {
		dongle.gain = nearest_gain(dongle.dev, dongle.gain);
		verbose_gain_set(dongle.dev, dongle.gain);
	}

	if (!custom_ppm) {
		verbose_ppm_eeprom(dongle.dev, &(dongle.ppm_error));
	}
	rtlsdr_set_agc_mode(dongle.dev, rtlagc);

	verbose_ppm_set(dongle.dev, dongle.ppm_error);

  verbose_set_bandwidth(dongle.dev, dongle.bandwidth);

	if (strcmp(output.filename, "-") == 0) { /* Write samples to stdout */
		output.file = stdout;
#ifdef _WIN32
		_setmode(_fileno(output.file), _O_BINARY);
#endif
	} else {
		output.file = fopen(output.filename, "wb");
		if (!output.file) {
			fprintf(stderr, "Failed to open %s\n", output.filename);
			exit(1);
		}
	}

	if (output.wav_format) {
		generate_header(&demod, &output);
	}

	//r = rtlsdr_set_testmode(dongle.dev, 1);

	/* Reset endpoint before we start reading from it (mandatory) */
	verbose_reset_buffer(dongle.dev);

	pthread_create(&controller.thread, NULL, controller_thread_fn, (void *)(&controller));
	usleep(100000);
	pthread_create(&output.thread, NULL, output_thread_fn, (void *)(&output));
	pthread_create(&demod.thread, NULL, demod_thread_fn, (void *)(&demod));
	if (output.lrmix) {
		pthread_create(&demod2.thread, NULL, demod_thread_fn, (void *)(&demod2));
	}
	pthread_create(&dongle.thread, NULL, dongle_thread_fn, (void *)(&dongle));

	while (!do_exit) {
		usleep(100000);
	}

	if (do_exit) {
		fprintf(stderr, "\nUser cancel, exiting...\n");}
	else {
		fprintf(stderr, "\nLibrary error %d, exiting...\n", r);}

	rtlsdr_cancel_async(dongle.dev);
	pthread_join(dongle.thread, NULL);
	safe_cond_signal(&demod.ready, &demod.ready_m);
	pthread_join(demod.thread, NULL);
	if (output.lrmix) {
		safe_cond_signal(&demod2.ready, &demod2.ready_m);
		pthread_join(demod2.thread, NULL);
	}
	safe_cond_signal(&output.results[0].ready, &output.results[0].ready_m);
	safe_cond_signal(&output.results[1].ready, &output.results[1].ready_m);
	pthread_join(output.thread, NULL);
	safe_cond_signal(&controller.hop, &controller.hop_m);
	pthread_join(controller.thread, NULL);

	//dongle_cleanup(&dongle);
	demod_cleanup(&demod);
	output_cleanup(&output);
	controller_cleanup(&controller);

	if (output.file != stdout) {
		fclose(output.file);}

	rtlsdr_close(dongle.dev);
	return r >= 0 ? r : -r;
}

// vim: tabstop=8:softtabstop=8:shiftwidth=8:noexpandtab
