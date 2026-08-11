/* X3F_PROCESS.C
 *
 * Library for processing X3F data.
 *
 * Copyright 2015 - Roland and Erik Karlsson
 * BSD-style - see doc/copyright.txt
 *
 */

#include "x3f_io.h"
#include "x3f_process.h"
#include "x3f_meta.h"
#include "x3f_image.h"
#include "x3f_matrix.h"
#include "x3f_denoise.h"
#include "x3f_spatial_gain.h"
#include "x3f_printf.h"

#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <assert.h>

static int sum_area(x3f_area16_t area, int colors,
		    uint64_t *sum)
{
  int row, col, color;

  for (color=0; color<colors; color++) sum[color] = 0;

  for (row = 0; row < area.rows; row++)
    for (col = 0; col < area.columns; col++)
      for (color = 0; color < colors; color++)
	sum[color] += (uint64_t)area.data[area.row_stride*row +
					  area.channels*col + color];

  return area.columns*area.rows;
}

static int sum_area_sqdev(x3f_area16_t area, int colors, double *mean,
			  double *sum)
{
  int row, col, color;

  for (color=0; color<colors; color++) sum[color] = 0.0;

  for (row = 0; row < area.rows; row++)
    for (col = 0; col < area.columns; col++)
      for (color = 0; color < colors; color++) {
	double dev = area.data[area.row_stride*row +
			       area.channels*col + color] - mean[color];
	sum[color] += dev*dev;
      }

  return area.columns*area.rows;
}

static int compare_u16(const void *left, const void *right)
{
  uint16_t a = *(const uint16_t *)left;
  uint16_t b = *(const uint16_t *)right;
  return (a > b) - (a < b);
}

/*
 * Photo Pro's F20 preprocessing does not combine every optical-black region.
 * It copies DarkShieldTop into a temporary array one plane at a time, selects
 * the 1st and 99th percentiles, and averages the central 98%.  In particular,
 * this avoids the illuminated right-hand reference columns present in some
 * Merrill frames.  Averaging those columns with the top shield moves the
 * black point by several raw codes, which the large signed Foveon matrix turns
 * into a strong shadow hue error.
 *
 * Reconstructed from FIPEngine2_F20::priProcessF20PreprocessStage at 0xa6ef8
 * through 0xa7fd8 in Photo Pro 6.9 (arm64).
 */
static int get_merrill_black_level(x3f_t *x3f, x3f_area16_t *image,
				   int rescale, int colors,
				   double *black_level, double *black_dev)
{
  x3f_area16_t shield;
  uint16_t *samples;
  size_t pixels, trim, begin, end, count, index;
  int row, col, color;

  if (colors != 3 ||
      !x3f_crop_area_camf(x3f, "DarkShieldTop", image, rescale, &shield))
    return 0;
  pixels = (size_t)shield.columns * shield.rows;
  if (pixels < 100) return 0;
  samples = malloc(pixels * sizeof(*samples));
  if (!samples) return 0;

  trim = (size_t)(pixels * 0.01);
  begin = trim;
  end = pixels - trim;
  count = end - begin;
  for (color = 0; color < colors; ++color) {
    double sum = 0.0, sqdev = 0.0;
    index = 0;
    for (row = 0; row < (int)shield.rows; ++row)
      for (col = 0; col < (int)shield.columns; ++col)
        samples[index++] = shield.data[shield.row_stride * row +
				       shield.channels * col + color];
    qsort(samples, pixels, sizeof(*samples), compare_u16);
    for (index = begin; index < end; ++index) sum += samples[index];
    black_level[color] = sum / count;
    for (index = begin; index < end; ++index) {
      double delta = samples[index] - black_level[color];
      sqdev += delta * delta;
    }
    black_dev[color] = sqrt(sqdev / count);
  }
  free(samples);
  x3f_printf(DEBUG,
	     "Merrill SPP dark-shield black = {%g,%g,%g}, dev = {%g,%g,%g}\n",
	     black_level[0], black_level[1], black_level[2],
	     black_dev[0], black_dev[1], black_dev[2]);
  return 1;
}

static int get_black_level(x3f_t *x3f,
			   x3f_area16_t *image, int rescale, int colors,
			   double *black_level, double *black_dev)
{
  uint64_t *black, *black_sum;
  double *black_sqdev, *black_sqdev_sum;
  int pixels_sum, i;

  col_side_t side[4] = {COL_SIDE_WRONG,
			COL_SIDE_WRONG,
			COL_SIDE_LEFT,
			COL_SIDE_RIGHT};
  char *name[4] = {"DarkShieldTop",
		   "DarkShieldBottom",
		   "Left", /* Only used in printout */
		   "Right" /* Only used in printout */
  };
  int use[4] = {1, 1, 1, 1};
  x3f_area16_t area[4];
  char *sensorid = NULL;

  if (x3f_get_prop_entry(x3f, "SENSORID", &sensorid) && sensorid &&
      !strcmp(sensorid, "F20") &&
      get_merrill_black_level(x3f, image, rescale, colors,
			      black_level, black_dev))
    return 1;

  if (image->channels < colors) return 0;

#define BOTTOM 1

  /* Workaround for bug in DP2 firmware. DarkShieldBottom is specified
     incorrectly and thus ignored. */
  {
    char *cammodel;

    if (x3f_get_prop_entry(x3f, "CAMMODEL", &cammodel))
      if (!strcmp(cammodel, "SIGMA DP2"))
	use[BOTTOM] = 0;
  }

  /* Workaround for bug in sd Quattro H firmaware. DarkShieldBottom is
     specified incorrectly and thus ignored. */
  {
    uint32_t cameraid;

    if (x3f_get_camf_unsigned(x3f, "CAMERAID", &cameraid))
      if (cameraid == X3F_CAMERAID_SDQH)
	use[BOTTOM] = 0;
  }

  /* Real CAMF rects */
  for (i=0; i<2; i++)
    if (use[i])
      use[i] = x3f_crop_area_camf(x3f, name[i], image, rescale, &area[i]);

  /* Column based rects */
  for (i=2; i<4; i++)
    if (use[i])
      use[i] = x3f_crop_area_column(x3f, side[i], image, rescale, &area[i]);

  for (i=0; i<4; i++)
    if (use[i])
      x3f_printf(DEBUG, "Calculate black level for %s\n", name[i]);
    else
      x3f_printf(DEBUG, "Do not calculate black level for %s\n", name[i]);

  pixels_sum = 0;
  black = alloca(colors*sizeof(uint64_t));
  black_sum = alloca(colors*sizeof(uint64_t));
  for (i=0; i<colors; i++) black_sum[i] = 0;

  x3f_printf(DEBUG, "Dark level\n");

  for (i=0; i<4; i++)
    if (use[i]) {
      int color;
      int pixels = sum_area(area[i], colors, black);

      pixels_sum += pixels;

      x3f_printf(DEBUG, "  %s (%d)\n", name[i], pixels);

      for (color = 0; color < colors; color++) {
	x3f_printf(DEBUG, "    mean[%d] = %f\n",
		   color,
		   (double)black[color]/pixels);
	black_sum[color] += black[color];
      }
    }

  if (pixels_sum == 0) return 0;

  for (i=0; i<colors; i++)
    black_level[i] = (double)black_sum[i]/pixels_sum;

  pixels_sum = 0;
  black_sqdev = alloca(colors*sizeof(double));
  black_sqdev_sum = alloca(colors*sizeof(double));
  for (i=0; i<colors; i++) black_sqdev_sum[i] = 0.0;

  for (i=0; i<4; i++)
    if (use[i]) {
      int color;
      int pixels = sum_area_sqdev(area[i], colors, black_level, black_sqdev);

      pixels_sum += pixels;

      for (color = 0; color < colors; color++) {
	black_sqdev_sum[color] += black_sqdev[color];
      }
    }

  if (pixels_sum == 0) return 0;

  x3f_printf(DEBUG, "  SUM\n");

  for (i=0; i<colors; i++) {
    black_dev[i] = sqrt(black_sqdev_sum[i]/pixels_sum);
    x3f_printf(DEBUG, "    level[%d] = %f\n",
	       i,
	       black_level[i]);
    x3f_printf(DEBUG, "    dev[%d] = %g\n",
	       i,
	       black_dev[i]);
  }

  return 1;
}

static void get_raw_neutral(double *raw_to_xyz, double *raw_neutral)
{
  double d65_xyz[3] = {0.95047, 1.00000, 1.08883};
  double xyz_to_raw[9];

  x3f_3x3_inverse(raw_to_xyz, xyz_to_raw);
  x3f_3x3_3x1_mul(xyz_to_raw, d65_xyz, raw_neutral);
}

/* extern */ int x3f_get_gain(x3f_t *x3f, char *wb, double *gain)
{
  double cam_to_xyz[9], wb_correction[9], gain_fact[3];

  if (x3f_get_camf_matrix_for_wb(x3f, "WhiteBalanceGains", wb, 3, 0, gain) ||
      x3f_get_camf_matrix_for_wb(x3f, "DP1_WhiteBalanceGains", wb, 3, 0, gain));
  else if (x3f_get_camf_matrix_for_wb(x3f, "WhiteBalanceIlluminants", wb,
				      3, 3, cam_to_xyz) &&
	   x3f_get_camf_matrix_for_wb(x3f, "WhiteBalanceCorrections", wb,
				      3, 3, wb_correction)) {
    double raw_to_xyz[9], raw_neutral[3];

    x3f_3x3_3x3_mul(wb_correction, cam_to_xyz, raw_to_xyz);
    get_raw_neutral(raw_to_xyz, raw_neutral);
    x3f_3x1_invert(raw_neutral, gain);
  }
  else
    return 0;

  if (x3f_get_camf_float_vector(x3f, "SensorAdjustmentGainFact", gain_fact))
    x3f_3x1_comp_mul(gain_fact, gain, gain);

  if (x3f_get_camf_float_vector(x3f, "TempGainFact", gain_fact))
    x3f_3x1_comp_mul(gain_fact, gain, gain);

  if (x3f_get_camf_float_vector(x3f, "FNumberGainFact", gain_fact))
    x3f_3x1_comp_mul(gain_fact, gain, gain);

  x3f_printf(DEBUG, "gain\n");
  x3f_3x1_print(DEBUG, gain);

  return 1;
}

/* extern */ int x3f_get_bmt_to_xyz(x3f_t *x3f, char *wb, double *bmt_to_xyz)
{
  double cc_matrix[9], cam_to_xyz[9], wb_correction[9];

  if (x3f_get_camf_matrix_for_wb(x3f, "WhiteBalanceColorCorrections", wb,
				 3, 3, cc_matrix) ||
      x3f_get_camf_matrix_for_wb(x3f, "DP1_WhiteBalanceColorCorrections", wb,
				 3, 3, cc_matrix)) {
    double srgb_to_xyz[9];

    x3f_sRGB_to_XYZ(srgb_to_xyz);
    x3f_3x3_3x3_mul(srgb_to_xyz, cc_matrix, bmt_to_xyz);
  }
  else if (x3f_get_camf_matrix_for_wb(x3f, "WhiteBalanceIlluminants", wb,
				      3, 3, cam_to_xyz) &&
	   x3f_get_camf_matrix_for_wb(x3f, "WhiteBalanceCorrections", wb,
				      3, 3, wb_correction)) {
    double raw_to_xyz[9], raw_neutral[3], raw_neutral_mat[9];

    x3f_3x3_3x3_mul(wb_correction, cam_to_xyz, raw_to_xyz);
    get_raw_neutral(raw_to_xyz, raw_neutral);
    x3f_3x3_diag(raw_neutral, raw_neutral_mat);
    x3f_3x3_3x3_mul(raw_to_xyz, raw_neutral_mat, bmt_to_xyz);
  }
  else
    return 0;

  x3f_printf(DEBUG, "bmt_to_xyz\n");
  x3f_3x3_print(DEBUG, bmt_to_xyz);

  return 1;
}

/* extern */ int x3f_get_raw_to_xyz(x3f_t *x3f, char *wb, double *raw_to_xyz)
{
  double bmt_to_xyz[9], gain[9], gain_mat[9];

  if (!x3f_get_gain(x3f, wb, gain)) return 0;
  if (!x3f_get_bmt_to_xyz(x3f, wb, bmt_to_xyz)) return 0;

  x3f_3x3_diag(gain, gain_mat);
  x3f_3x3_3x3_mul(bmt_to_xyz, gain_mat, raw_to_xyz);

  x3f_printf(DEBUG, "raw_to_xyz\n");
  x3f_3x3_print(DEBUG, raw_to_xyz);

  return 1;
}

/* x3f_denoise expects a 14-bit image since rescaling by a factor of 4
   takes place internally. */
#define INTERMEDIATE_DEPTH 14
#define INTERMEDIATE_UNIT ((1<<INTERMEDIATE_DEPTH) - 1)
#define INTERMEDIATE_BIAS_FACTOR 4.0

static int get_max_intermediate(x3f_t *x3f, char *wb,
				double intermediate_bias,
				uint32_t *max_intermediate)
{
  double gain[3], maxgain = 0.0;
  int i;

  if (!x3f_get_gain(x3f, wb, gain)) return 0;

  /* Cap the gains to 1.0 to avoid clipping */
  for (i=0; i<3; i++)
    if (gain[i] > maxgain) maxgain = gain[i];
  for (i=0; i<3; i++)
    max_intermediate[i] =
      (int32_t)round(gain[i]*(INTERMEDIATE_UNIT - intermediate_bias)/maxgain +
		     intermediate_bias);

  return 1;
}

static int get_intermediate_bias(x3f_t *x3f, char *wb,
				 double *black_level, double *black_dev,
				 double *intermediate_bias)
{
  uint32_t max_raw[3], max_intermediate[3];
  int i;

  if (!x3f_get_max_raw(x3f, max_raw)) return 0;
  if (!get_max_intermediate(x3f, wb, 0, max_intermediate)) return 0;

  *intermediate_bias = 0.0;
  for (i=0; i<3; i++) {
    double bias = INTERMEDIATE_BIAS_FACTOR * black_dev[i] *
      max_intermediate[i] / (max_raw[i] - black_level[i]);
    if (bias > *intermediate_bias) *intermediate_bias = bias;
  }

  return 1;
}

typedef struct bad_pixel_s {
  int c, r;
  uint16_t f20_flags;
  struct bad_pixel_s *prev, *next;
} bad_pixel_t;

typedef struct {
  /* c = column, r = row; i = intial, f = final, p = pitch, s = size */
  int ci, cf, cp, cs, ri, rf, rp, rs;
} grid_t;

/* Select Photo Pro's exposure/temperature-dependent F20 defect class.  CAMF
   BadnessTable rows are {temperature coefficient, exposure threshold, class}.
   The threshold is exponential in sensor temperature and compared with the
   shutter exposure expressed in seconds. */
static unsigned int merrill_badness_class(x3f_t *x3f)
{
  static const double fallback[] = {
    30.0, 10000.0, 0.0, 30.0, 3000.0, 1.0,
    30.0,   500.0, 2.0, 30.0,  200.0, 3.0
  };
  double *table = (double *)fallback;
  double shutter = 0.0, temperature = 0.0, metric;
  int rows = 4, columns = 3, row;
  void *camf_table = NULL;

  if (x3f_get_camf_matrix_var(x3f, "BadnessTable", &rows, &columns, NULL,
			      M_FLOAT, &camf_table) && rows > 0 && columns == 3)
    table = camf_table;
  else {
    rows = 4;
    columns = 3;
  }
  x3f_get_camf_float(x3f, "CaptureShutter", &shutter);
  x3f_get_camf_float(x3f, "SensorTemperature", &temperature);
  metric = exp(temperature * 0.086643) * (shutter / 1000.0);
  for (row = 0; row < rows; ++row) {
    double threshold = table[3 * row + 1] *
      exp(table[3 * row] * 0.086643);
    if (metric >= threshold)
      return (unsigned int)fmax(0.0, table[3 * row + 2]);
  }
  return (unsigned int)fmax(0.0, table[3 * (rows - 1) + 2]);
}

static unsigned int merrill_badness_cutoff(x3f_t *x3f, const char *offset)
{
  unsigned int badness = merrill_badness_class(x3f), adjustment = 1;
  if (getenv("FAST_SIGMA_DISABLE_BADNESS_GATING")) return 0;
  x3f_get_camf_unsigned(x3f, (char *)offset, &adjustment);
  return badness > adjustment ? badness - adjustment : 0;
}

/* Address pixel at column _c and row _r */
#define _PN(_c, _r, _cs) ((_r)*(_cs) + (_c))

/* Test if a pixel (_c,_r) is within a rectancle */
#define _INB(_c, _r, _cs, _rs)					\
  ((_c) >= 0 && (_c) < (_cs) && (_r) >= 0 && (_r) < (_rs))

/* Test if a pixel has been marked in the bad pixel vector */
#define TEST_PIX(_vec, _c, _r, _cs, _rs)				\
  (_INB((_c), (_r), (_cs), (_rs)) ?					\
   (_vec)[_PN((_c), (_r), (_cs)) >> 5] &				\
   1 << (_PN((_c), (_r), (_cs)) & 0x1f) : 1)

/* Mark the pixel, in the bad pixel vector and the bad pixel list */
#define MARK_PIX(_list, _vec, _c, _r, _cs, _rs)				\
  do {									\
    if (!TEST_PIX((_vec), (_c), (_r), (_cs), (_rs))) {			\
      bad_pixel_t *_p = malloc(sizeof(bad_pixel_t));			\
      _p->c = (_c);							\
      _p->r = (_r);							\
      _p->f20_flags = 0;						\
      _p->prev = NULL;							\
      _p->next = (_list);						\
      if (_list) (_list)->prev = (_p);					\
      (_list) = _p;							\
      (_vec)[_PN((_c), (_r), (_cs)) >> 5] |=				\
	1 << (_PN((_c), (_r), (_cs)) & 0x1f);				\
    }									\
    else if (!_INB((_c), (_r), (_cs), (_rs)))				\
      x3f_printf(WARN, "Bad pixel (%u,%u) out of bounds : (%u,%u)\n",   \
		 (_c), (_r), (_cs), (_rs));				\
  } while (0)

/* Clear the mark in the bad pixel vector */
#define CLEAR_PIX(_vec, _c, _r, _cs, _rs)				\
  do {									\
    assert(_INB((_c), (_r), (_cs), (_rs)));				\
    _vec[_PN((_c), (_r), (_cs)) >> 5] &=				\
      ~(1 << (_PN((_c), (_r), (_cs)) & 0x1f));				\
  } while (0)

/*
 * F20 bad-pixel records contain an eight-bit mask for the pixels surrounding
 * the recorded centre.  Photo Pro does not blindly replace those neighbours:
 * DetectBadReds tests them against the camera's quadratic ColorNoiseModel and
 * only appends genuine bottom-layer (red) excursions to the repair list.
 *
 * At this point x3f-tools has already mapped each plane to the intermediate
 * domain.  Undo that affine mapping for the detector so its test is performed
 * in the same pre-gain sensor-code domain as Sigma's routine.
 */
static void mark_dynamic_red_neighbors(bad_pixel_t **list, uint32_t *vec,
				       x3f_area16_t *image,
				       const uint32_t *record,
				       const double *scale,
				       double intermediate_bias,
				       const double *noise_model,
				       unsigned int badness_cutoff,
				       int *detected)
{
  static const int dc[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
  static const int dr[8] = {-1,-1,-1,  0, 0,  1, 1, 1};
  unsigned int mask = record[2] & 0xffu;
  int center_row = (int)record[0], center_col = (int)record[1];
  int bit;

  if (!noise_model || badness_cutoff > 3 || !scale ||
      scale[0] <= 0.0 || scale[2] <= 0.0)
    return;
  for (bit = 0; bit < 8; ++bit) {
    int row, col;
    const uint16_t *pixel;
    double bottom, top, bottom_noise, top_noise;
    unsigned int available = 0;
    bad_pixel_t *p;
    if (!(mask & (1u << bit))) continue;
    row = center_row + dr[bit];
    col = center_col + dc[bit];
    if (!_INB(col, row, image->columns, image->rows) ||
	TEST_PIX(vec, col, row, image->columns, image->rows))
      continue;
    pixel = image->data + row * image->row_stride + col * image->channels;
    bottom = (pixel[0] - intermediate_bias) / scale[0];
    top = (pixel[2] - intermediate_bias) / scale[2];
    bottom_noise = noise_model[0] + bottom * noise_model[3] +
	bottom * bottom * noise_model[6];
    top_noise = noise_model[2] + top * noise_model[5] +
	top * top * noise_model[8];
    if (fmax(bottom, 0.0) - bottom_noise <=
	top_noise + fmax(top, 0.0))
      continue;

    /* Reproduce the surviving-neighbour mask carried by DetectBadReds.
       Static F20 centers have already been collected, so none can become an
       input to the plane reconstruction.  Previously detected dynamic reds
       are excluded for the same reason. */
    {
      int neighbor;
      for (neighbor = 0; neighbor < 8; ++neighbor) {
	int nr = row + dr[neighbor], nc = col + dc[neighbor];
	if (_INB(nc, nr, image->columns, image->rows) &&
	    !TEST_PIX(vec, nc, nr, image->columns, image->rows))
	  available |= 1u << neighbor;
      }
    }
    if (!available) continue;

    p = malloc(sizeof(*p));
    if (!p) continue;
    p->c = col;
    p->r = row;
    /* DetectBadReds emits badness 3 and repair type 4 (bottom plane). */
    p->f20_flags = (uint16_t)(0x4300u | available);
    p->prev = NULL;
    p->next = *list;
    if (*list) (*list)->prev = p;
    *list = p;
    vec[_PN(col, row, image->columns) >> 5] |=
      1u << (_PN(col, row, image->columns) & 0x1f);
    ++*detected;
  }
}

static void mark_f20_record(bad_pixel_t **list, uint32_t *vec,
			    int col, int row, uint16_t flags,
			    int columns, int rows)
{
  bad_pixel_t *p;
  if (!_INB(col, row, columns, rows) || TEST_PIX(vec, col, row, columns, rows))
    return;
  p = malloc(sizeof(*p));
  if (!p) return;
  p->c = col; p->r = row; p->f20_flags = flags;
  p->prev = NULL; p->next = *list;
  if (*list) (*list)->prev = p;
  *list = p;
  vec[_PN(col, row, columns) >> 5] |=
    1u << (_PN(col, row, columns) & 0x1f);
}

/* Exact seven-way ReplaceBadPixelsF20 plane reconstruction.  The low byte
   selects valid samples in the surrounding 3x3 ring.  With three or more
   samples Sigma discards each plane's minimum and maximum independently,
   then reconstructs only the plane(s) named by the high-nibble repair type. */
static int repair_f20_pixel(x3f_area16_t *image, const bad_pixel_t *bad)
{
  static const int dc[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
  static const int dr[8] = {-1,-1,-1,  0, 0,  1, 1, 1};
  unsigned int mask = bad->f20_flags & 0xffu;
  unsigned int type = ((bad->f20_flags >> 12) & 0xfu) - 1u;
  double sum[3] = {0.0, 0.0, 0.0};
  uint16_t low[3] = {65535,65535,65535}, high[3] = {0,0,0};
  uint16_t *center;
  unsigned int count = 0, bit, channel;
  double mean[3], result[3];
  if (!_INB(bad->c, bad->r, image->columns, image->rows) || !mask || type > 6)
    return 0;
  for (bit = 0; bit < 8; ++bit) {
    const uint16_t *sample;
    int row, col;
    if (!(mask & (1u << bit))) continue;
    row = bad->r + dr[bit]; col = bad->c + dc[bit];
    if (!_INB(col, row, image->columns, image->rows)) continue;
    sample = image->data + row * image->row_stride + col * image->channels;
    for (channel = 0; channel < 3; ++channel) {
      sum[channel] += sample[channel];
      if (sample[channel] < low[channel]) low[channel] = sample[channel];
      if (sample[channel] > high[channel]) high[channel] = sample[channel];
    }
    ++count;
  }
  if (!count) return 0;
  if (count > 2) {
    for (channel = 0; channel < 3; ++channel)
      sum[channel] -= low[channel] + high[channel];
    count -= 2;
  }
  for (channel = 0; channel < 3; ++channel) mean[channel] = sum[channel] / count;
  center = image->data + bad->r * image->row_stride +
    bad->c * image->channels;
  for (channel = 0; channel < 3; ++channel) result[channel] = center[channel];
  switch (type) {
  case 0:
    result[2] = 0.5 * ((center[0] + mean[2] - mean[0]) +
		       (center[1] + mean[2] - mean[1]));
    break;
  case 1:
    result[1] = 0.5 * ((center[0] + mean[1] - mean[0]) +
		       (center[2] + mean[1] - mean[2]));
    break;
  case 2:
    result[1] = center[0] + mean[1] - mean[0];
    result[2] = center[0] + mean[2] - mean[0];
    break;
  case 3:
    result[0] = 0.5 * ((center[1] + mean[0] - mean[1]) +
		       (center[2] + mean[0] - mean[2]));
    break;
  case 4:
    result[0] = center[1] + mean[0] - mean[1];
    result[2] = center[1] + mean[2] - mean[1];
    break;
  case 5:
    result[0] = center[2] + mean[0] - mean[2];
    result[1] = center[2] + mean[1] - mean[2];
    break;
  case 6:
    for (channel = 0; channel < 3; ++channel) result[channel] = mean[channel];
    break;
  }
  for (channel = 0; channel < 3; ++channel) {
    long value = lrint(result[channel]);
    center[channel] = (uint16_t)(value < 0 ? 0 : value > 65535 ? 65535 : value);
  }
  return 1;
}

static void interpolate_bad_pixels(x3f_t *x3f, x3f_area16_t *image,
				   int colors, const double *scale,
				   double intermediate_bias)
{
  bad_pixel_t *bad_pixel_list = NULL;
  uint32_t *bad_pixel_vec = calloc((image->rows*image->columns + 31)/32,
				   sizeof(uint32_t));
  int row, col, color, i;
  uint32_t *bpf23, cameraid;
  int bpf23_len;
  int stat_pass = 0;		/* Statistics */
  int fix_corner = 0;		/* By default, do not accept corners */
  int dynamic_reds = 0;
  unsigned int badness_cutoff = merrill_badness_cutoff(x3f,
						       "BPBadnessOffset");
  double noise_model[9];
  int have_noise_model = colors == 3 &&
    !getenv("FAST_SIGMA_DISABLE_DYNAMIC_DEFECTS") &&
    x3f_get_camf_matrix(x3f, "ColorNoiseModel", 3, 3, 0, M_FLOAT,
			noise_model);

  /* BEGIN - collecting bad pixels. This part reads meta data and
     collects all bad pixels both in the list 'bad_pixel_list' and the
     vector 'bad_pixel_vec' */

  if (colors == 3) {
    uint32_t keep[4], hpinfo[4], *bp, *bpf20 = NULL, *jpeg_f20 = NULL;
    int bp_num, bpf20_rows = 0, bpf20_cols = 0;
    int jpeg_f20_rows = 0, jpeg_f20_cols = 0;

    if (x3f_get_camf_matrix(x3f, "KeepImageArea", 4, 0, 0, M_UINT, keep) &&
	x3f_get_camf_matrix_var(x3f, "BadPixels", &bp_num, NULL, NULL,
				M_UINT, (void **)&bp))
      for (i=0; i < bp_num; i++)
	MARK_PIX(bad_pixel_list, bad_pixel_vec,
		 ((bp[i] & 0x000fff00) >> 8) - keep[0],
		 ((bp[i] & 0xfff00000) >> 20) - keep[1],
		 image->columns, image->rows);

    /* NOTE: the numbers of rows and cols in this matrix are
       interchanged due to bug in camera firmware */
    if (x3f_get_camf_matrix_var(x3f, "BadPixelsF20",
				&bpf20_cols, &bpf20_rows, NULL,
				M_UINT, (void **)&bpf20) && bpf20_cols == 3) {
      for (row=0; row < bpf20_rows; row++)
	if (((bpf20[3 * row + 2] >> 8) & 0xfu) >= badness_cutoff)
	  mark_f20_record(&bad_pixel_list, bad_pixel_vec,
			  bpf20[3*row + 1], bpf20[3*row + 0],
			  (uint16_t)bpf20[3*row + 2],
			  image->columns, image->rows);
    } else {
      bpf20 = NULL;
      bpf20_rows = 0;
    }

    /* NOTE: the numbers of rows and cols in this matrix are
       interchanged due to bug in camera firmware
       TODO: should Jpeg_BadClutersF20 really be used for RAW? It works
       though. */
    if (x3f_get_camf_matrix_var(x3f, "Jpeg_BadClusters",
				&jpeg_f20_cols, &jpeg_f20_rows, NULL,
				M_UINT, (void **)&jpeg_f20) && jpeg_f20_cols == 3) {
      for (row=0; row < jpeg_f20_rows; row++)
	if (((jpeg_f20[3 * row + 2] >> 8) & 0xfu) >= badness_cutoff)
	  mark_f20_record(&bad_pixel_list, bad_pixel_vec,
			  jpeg_f20[3*row + 1], jpeg_f20[3*row + 0],
			  (uint16_t)jpeg_f20[3*row + 2],
			  image->columns, image->rows);
    } else {
      jpeg_f20 = NULL;
      jpeg_f20_rows = 0;
    }

    /* Detect red-only excursions only after every recorded center is marked.
       This preserves the static record's repair type when lists overlap and
       lets DetectBadReds construct a mask that excludes all known defects. */
    if (have_noise_model) {
      for (row=0; row < bpf20_rows; row++)
	mark_dynamic_red_neighbors(&bad_pixel_list, bad_pixel_vec, image,
				   &bpf20[3 * row], scale,
				   intermediate_bias, noise_model,
				   badness_cutoff, &dynamic_reds);
      for (row=0; row < jpeg_f20_rows; row++)
	mark_dynamic_red_neighbors(&bad_pixel_list, bad_pixel_vec, image,
				   &jpeg_f20[3 * row], scale,
				   intermediate_bias, noise_model,
				   badness_cutoff, &dynamic_reds);
    }

    /* TODO: should those really be interpolated over, or should they be
       rescaled instead? */
    if (x3f_get_camf_matrix(x3f, "HighlightPixelsInfo", 2, 2, 0, M_UINT,
			    hpinfo))
      for (row = hpinfo[1]; row < image->rows; row += hpinfo[3])
	for (col = hpinfo[0]; col < image->columns; col += hpinfo[2])
	  MARK_PIX(bad_pixel_list, bad_pixel_vec,
		   col, row, image->columns, image->rows);
  } /* colors == 3 */

  if ((colors == 1 && x3f_get_camf_matrix_var(x3f, "BadPixelsLumaF23",
					      &bpf23_len, NULL, NULL,
					      M_UINT, (void **)&bpf23)) ||
      (colors == 3 && x3f_get_camf_matrix_var(x3f, "BadPixelsChromaF23",
					      &bpf23_len, NULL, NULL,
					      M_UINT, (void **)&bpf23)))
    for (i=0, row=-1; i < bpf23_len; i++)
      if (row == -1) row = bpf23[i];
      else if (bpf23[i] == 0) row = -1;
      else {MARK_PIX(bad_pixel_list, bad_pixel_vec,
		     bpf23[i], row,
		     image->columns, image->rows); i++;}

  /* Interpolate over autofocus pixels for sd Quattro and sd Quattro H.
     TODO: The positions shouldn't really be hardcoded. */

  if (x3f_get_camf_unsigned(x3f, "CAMERAID", &cameraid)) {
    const grid_t *g = NULL;

    if (cameraid == X3F_CAMERAID_SDQ) {
      static const grid_t sdq_af_luma    = {217, 5641, 16, 1, 464, 3312, 32, 2};
      static const grid_t sdq_af_chroma  = {108, 2820,  8, 1, 232, 1656, 16, 1};
      g = (colors == 1 ? &sdq_af_luma : &sdq_af_chroma);
    } else if (cameraid == X3F_CAMERAID_SDQH) {
      static const grid_t sdqh_af_luma   = {233, 6425, 16, 1, 592, 3888, 32, 2};
      static const grid_t sdqh_af_chroma = {116, 2820,  8, 1, 296, 1944, 16, 1};
      g = (colors == 1 ? &sdqh_af_luma : &sdqh_af_chroma);
    }

    if (g != NULL) {
      int r, c;

      x3f_printf(DEBUG, "Create AF grid for removing bad pixels\n");

      for (row = g->ri; row <= g->rf; row += g->rp)
	for (col = g->ci; col <= g->cf; col += g->cp)
	  for (r = 0; r < g->rs; r++)
	    for (c = 0; c < g->cs; c++)
	      MARK_PIX(bad_pixel_list, bad_pixel_vec, col+c, row+r,
		       image->columns, image->rows);
    }
  }

  /* END - collecting bad pixels */


  /* BEGIN - fixing bad pixels. This part fixes all bad pixels
     collected in the list 'bad_pixel_list', using the mirror data in
     the vector 'bad_pixel_vec'.  This is made in passes. In each pass
     all pixels that can be interpolated are interpolated and also
     removed from the list of bad pixels.  Eventually the list of bad
     pixels is going to be empty. */

  if (bad_pixel_list)
    x3f_printf(DEBUG, "There are bad pixels to fix\n");
  if (dynamic_reds)
    x3f_printf(DEBUG, "Merrill dynamic red detector added %d pixels\n",
	       dynamic_reds);
  x3f_printf(DEBUG, "Merrill bad-pixel class cutoff = %u\n",
	     badness_cutoff);

  while (bad_pixel_list) {
    bad_pixel_t *p, *pn;
    bad_pixel_t *fixed = NULL;	/* Contains all, in this pass, fixed pixels */
    struct {
      int f20, all_four, two_linear, two_corner, left; /* Statistics */
    } stats = {0,0,0,0,0};

    /* Iterate over all pixels in the bad pixel list, in this pass */
    for (p=bad_pixel_list; p && (pn=p->next, 1); p=pn) {
      uint16_t *outp =
	&image->data[p->r*image->row_stride + p->c*image->channels];
      uint16_t *inp[4] = {NULL, NULL, NULL, NULL};
      int num = 0;

      if (p->f20_flags && repair_f20_pixel(image, p)) {
	stats.f20++;
	if (p->prev) p->prev->next = p->next;
	else bad_pixel_list = p->next;
	if (p->next) p->next->prev = p->prev;
	p->prev = NULL; p->next = fixed; fixed = p;
	continue;
      }

      /* Collect status of neighbor pixels */
      if (!TEST_PIX(bad_pixel_vec, p->c - 1, p->r, image->columns, image->rows))
	num++, inp[0] =
	  &image->data[p->r*image->row_stride + (p->c - 1)*image->channels];
      if (!TEST_PIX(bad_pixel_vec, p->c + 1, p->r, image->columns, image->rows))
	num++, inp[1] =
	  &image->data[p->r*image->row_stride + (p->c + 1)*image->channels];
      if (!TEST_PIX(bad_pixel_vec, p->c, p->r - 1, image->columns, image->rows))
	num++, inp[2] =
	  &image->data[(p->r - 1)*image->row_stride + p->c*image->channels];
      if (!TEST_PIX(bad_pixel_vec, p->c, p->r + 1, image->columns, image->rows))
	num++, inp[3] =
	  &image->data[(p->r + 1)*image->row_stride + p->c*image->channels];

      /* Test if interpolation is possible ... */
      if (inp[0] && inp[1] && inp[2] && inp[3])
	/* ... all four neighbors are OK */
	stats.all_four++;
      else if (inp[0] && inp[1])
	/* ... left and right are OK */
	inp[2] = inp[3] = NULL, num = 2, stats.two_linear++;
      else if (inp[2] && inp[3])
	/* ... above and under are OK */
	inp[0] = inp[1] = NULL, num = 2, stats.two_linear++;
      else if (fix_corner && num == 2)
	/* ... corner (plus nothing else to do) are OK */
	stats.two_corner++;
      else
	/* ... nope - it was not possible. Look at next without doing
	   interpolation.  */
	{stats.left++; continue;};

      /* Interpolate the actual pixel */
      for (color=0; color < colors; color++) {
	uint32_t sum = 0;
	for (i=0; i<4; i++)
	  if (inp[i]) sum += inp[i][color];
	outp[color] = (sum + num/2)/num;
      }

      /* Remove p from bad_pixel_list */
      if (p->prev) p->prev->next  = p->next;
      else         bad_pixel_list = p->next;
      if (p->next) p->next->prev = p->prev;

      /* Add p to fixed list */
      p->prev = NULL;
      p->next = fixed;
      fixed = p;
    }

    x3f_printf(DEBUG, "Bad pixels pass %d: %d fixed (%d F20, %d all_four, %d linear, %d corner), %d left\n",
	       stat_pass,
	       stats.f20 + stats.all_four + stats.two_linear + stats.two_corner,
	       stats.f20,
	       stats.all_four,
	       stats.two_linear,
	       stats.two_corner,
	       stats.left);

    if (!fixed) {
      /* If nothing else to do, accept corners */
      if (!fix_corner) fix_corner = 1;
      else {
	x3f_printf(WARN, "Failed to interpolate %d bad pixels\n",
		   stats.left);
	fixed = bad_pixel_list;	/* Free remaining list entries */
	bad_pixel_list = NULL;	/* Force termination */
      }
    }

    /* Clear the bad pixel vector and free the list */
    for (p=fixed; p && (pn=p->next, 1); p=pn) {
      CLEAR_PIX(bad_pixel_vec, p->c, p->r, image->columns, image->rows);
      free(p);
    }

    stat_pass++;
  }

  /* END - fixing bad pixels */

  free(bad_pixel_vec);
}

typedef struct {
  int row, col;
  unsigned int channels;
  int pending;
} merrill_cluster_pixel_t;

static unsigned int cluster_bad_channels(const merrill_cluster_pixel_t *bad,
					  unsigned int count,
					  int row, int col)
{
  unsigned int i;
  for (i = 0; i < count; ++i)
    if (bad[i].pending && bad[i].row == row && bad[i].col == col)
      return bad[i].channels;
  return 0;
}

/* Repair the compact BadClustersF20 stream.  Each record selects one of four
   exposure-dependent cluster variants, describes a <=25x25 rectangle, then
   packs {row:6,col:6,channel-mask:4} defects.  Photo Pro ranks replacement
   patches by RGB gradient.  This implementation performs the equivalent
   edge-aware operation iteratively, taking the lowest-gradient valid axis
   through each defect before allowing that result to support its neighbors. */
static void repair_merrill_bad_clusters(x3f_t *x3f, x3f_area16_t *image)
{
  uint32_t *words;
  int rows, columns;
  size_t cursor = 0, word_count;
  unsigned int cutoff = merrill_badness_cutoff(x3f, "BCBadnessOffset");
  unsigned int default_margin = 25, repaired_records = 0, repaired_pixels = 0;

  if (getenv("FAST_SIGMA_DISABLE_BAD_CLUSTERS") || image->channels < 3 ||
      !x3f_get_camf_matrix_var(x3f, "BadClustersF20", &rows, &columns,
			       NULL, M_UINT, (void **)&words))
    return;
  x3f_get_camf_unsigned(x3f, "BadClusterMargin", &default_margin);
  word_count = (size_t)rows * columns;
  while (cursor + 4 <= word_count) {
    uint32_t descriptor = words[cursor];
    int base_row = (int)words[cursor + 1];
    int base_col = (int)words[cursor + 2];
    uint32_t control = words[cursor + 3];
    unsigned int count = control & 0xffu;
    unsigned int record_class = descriptor >> 14;
    unsigned int margin = (control >> 8) & 0xffu;
    merrill_cluster_pixel_t bad[255];
    unsigned int i, remaining;

    if (cursor + 4 + count > word_count) break;
    if (record_class != cutoff || count == 0) {
      cursor += 4 + count;
      continue;
    }
    if (margin > default_margin) margin = default_margin;
    if (margin < 5) margin = 5;
    for (i = 0; i < count; ++i) {
      uint32_t packed = words[cursor + 4 + i];
      bad[i].row = base_row + (int)(packed >> 10);
      bad[i].col = base_col + (int)((packed >> 4) & 0x3fu);
      bad[i].channels = packed & 7u;
      bad[i].pending = bad[i].channels != 0 &&
	_INB(bad[i].col, bad[i].row, image->columns, image->rows);
    }
    remaining = count;
    while (remaining) {
      static const int dr[4] = {0, 1, 1, 1};
      static const int dc[4] = {1, 0, 1,-1};
      float best_record_score = INFINITY;
      int best_pixel = -1, best_direction = -1;
      int best_r0 = 0, best_c0 = 0, best_r1 = 0, best_c1 = 0;
      for (i = 0; i < count; ++i) {
	unsigned int direction;
	if (!bad[i].pending) continue;
	for (direction = 0; direction < 4; ++direction) {
	  int distance0, distance1, r0 = bad[i].row, c0 = bad[i].col;
	  int r1 = r0, c1 = c0;
	  unsigned int channel;
	  float score = 0.0f;
	  const uint16_t *p0, *p1;
	  for (distance0 = 1; distance0 <= (int)margin; ++distance0) {
	    r0 = bad[i].row - dr[direction] * distance0;
	    c0 = bad[i].col - dc[direction] * distance0;
	    if (!_INB(c0, r0, image->columns, image->rows)) continue;
	    if (!(cluster_bad_channels(bad, count, r0, c0) &
		  bad[i].channels)) break;
	  }
	  for (distance1 = 1; distance1 <= (int)margin; ++distance1) {
	    r1 = bad[i].row + dr[direction] * distance1;
	    c1 = bad[i].col + dc[direction] * distance1;
	    if (!_INB(c1, r1, image->columns, image->rows)) continue;
	    if (!(cluster_bad_channels(bad, count, r1, c1) &
		  bad[i].channels)) break;
	  }
	  if (distance0 > (int)margin || distance1 > (int)margin ||
	      !_INB(c0, r0, image->columns, image->rows) ||
	      !_INB(c1, r1, image->columns, image->rows))
	    continue;
	  p0 = image->data + r0 * image->row_stride + c0 * image->channels;
	  p1 = image->data + r1 * image->row_stride + c1 * image->channels;
	  for (channel = 0; channel < 3; ++channel)
	    score += fabsf((float)p0[channel] - p1[channel]);
	  score *= 1.0f + 0.05f * (distance0 + distance1 - 2);
	  if (score < best_record_score) {
	    best_record_score = score;
	    best_pixel = (int)i;
	    best_direction = (int)direction;
	    best_r0 = r0; best_c0 = c0; best_r1 = r1; best_c1 = c1;
	  }
	}
      }
      if (best_pixel < 0) break;
      {
	merrill_cluster_pixel_t *target = &bad[best_pixel];
	uint16_t *out = image->data + target->row * image->row_stride +
	  target->col * image->channels;
	const uint16_t *p0 = image->data + best_r0 * image->row_stride +
	  best_c0 * image->channels;
	const uint16_t *p1 = image->data + best_r1 * image->row_stride +
	  best_c1 * image->channels;
	int d0 = abs(target->row - best_r0) + abs(target->col - best_c0);
	int d1 = abs(target->row - best_r1) + abs(target->col - best_c1);
	unsigned int channel;
	(void)best_direction;
	for (channel = 0; channel < 3; ++channel)
	  if (target->channels & (1u << channel))
	    out[channel] = (uint16_t)((p0[channel] * d1 + p1[channel] * d0 +
				       (d0 + d1) / 2) / (d0 + d1));
	target->pending = 0;
	--remaining;
	++repaired_pixels;
      }
    }
    ++repaired_records;
    cursor += 4 + count;
  }
  x3f_printf(DEBUG,
    "Merrill bad clusters: class=%u records=%u repaired_pixels=%u\n",
    cutoff, repaired_records, repaired_pixels);
}

static int preprocess_data(x3f_t *x3f, int fix_bad, char *wb,
			   x3f_image_levels_t *ilevels,
			   double *native_per_intermediate)
{
  x3f_area16_t image, qtop;
  int row, col, color;
  uint32_t max_raw[3];
  double scale[3], gain[3], black_level[3], black_dev[3], intermediate_bias;
  int quattro = x3f_image_area_qtop(x3f, &qtop);
  int colors_in = quattro ? 2 : 3;

  if (!x3f_image_area(x3f, &image) || image.channels < 3) return 0;
  if (quattro && (qtop.channels < 1 ||
		  qtop.rows < 2*image.rows || qtop.columns < 2*image.columns))
    return 0;

  if (!get_black_level(x3f, &image, 1, colors_in, black_level, black_dev) ||
      (quattro && !get_black_level(x3f, &qtop, 0, 1,
				   &black_level[2], &black_dev[2]))) {
    x3f_printf(ERR, "Could not get black level\n");
    return 0;
  }
  x3f_printf(DEBUG, "black_level = {%g,%g,%g}, black_dev = {%g,%g,%g}\n",
	     black_level[0], black_level[1], black_level[2],
	     black_dev[0], black_dev[1], black_dev[2]);

  if (!x3f_get_max_raw(x3f, max_raw)) {
    x3f_printf(ERR, "Could not get maximum RAW level\n");
    return 0;
  }
  x3f_printf(DEBUG, "max_raw = {%u,%u,%u}\n",
	     max_raw[0], max_raw[1], max_raw[2]);

  if (!get_intermediate_bias(x3f, wb, black_level, black_dev,
			     &intermediate_bias)) {
    x3f_printf(ERR, "Could not get intermediate bias\n");
    return 0;
  }
  x3f_printf(DEBUG, "intermediate_bias = %g\n", intermediate_bias);
  ilevels->black[0] = ilevels->black[1] = ilevels->black[2] = intermediate_bias;
  x3f->merrill_intermediate_bias = intermediate_bias;

  if (!get_max_intermediate(x3f, wb, intermediate_bias, ilevels->white)) {
    x3f_printf(ERR, "Could not get maximum intermediate level\n");
    return 0;
  }
  x3f_printf(DEBUG, "max_intermediate = {%u,%u,%u}\n",
	     ilevels->white[0], ilevels->white[1], ilevels->white[2]);

  for (color = 0; color < 3; color++)
    scale[color] = (ilevels->white[color] - ilevels->black[color]) /
      (max_raw[color] - black_level[color]);

  /* Photo Pro's preprocess image is floating point and ApplyGains leaves it
     in gain-scaled sensor-code units.  x3f-tools uses a biased 14-bit integer
     representation instead.  Preserve the exact affine conversion so later
     recovered float-domain detectors can use Sigma's native thresholds. */
  if (native_per_intermediate) {
    if (!x3f_get_gain(x3f, wb, gain)) return 0;
    for (color = 0; color < 3; ++color)
      native_per_intermediate[color] = scale[color] > 0.0
	? gain[color] / scale[color] : 0.0;
  }

  /* Preprocess image data (HUF/TRU->x3rgb16) */
  for (row = 0; row < image.rows; row++)
    for (col = 0; col < image.columns; col++)
      for (color = 0; color < colors_in; color++) {
	uint16_t *valp =
	  &image.data[image.row_stride*row + image.channels*col + color];
	int32_t out =
	  (int32_t)round(scale[color] * (*valp - black_level[color]) +
			 ilevels->black[color]);

	if (out < 0) *valp = 0;
	else if (out > 65535) *valp = 65535;
	else *valp = out;
      }

  if (quattro) {
    /* Preprocess and downsample Quattro top layer (Q->top16) */
    for (row = 0; row < image.rows; row++)
      for (col = 0; col < image.columns; col++) {
	uint16_t *outp =
	  &image.data[image.row_stride*row + image.channels*col + 2];
	uint16_t *row1 =
	  &qtop.data[qtop.row_stride*2*row + qtop.channels*2*col];
	uint16_t *row2 =
	  &qtop.data[qtop.row_stride*(2*row+1) + qtop.channels*2*col];
	uint32_t sum =
	  row1[0] + row1[qtop.channels] + row2[0] + row2[qtop.channels];
	int32_t out = (int32_t)round(scale[2] * (sum/4.0 - black_level[2]) +
				     ilevels->black[2]);

	if (out < 0) *outp = 0;
	else if (out > 65535) *outp = 65535;
	else *outp = out;
      }

    /* Preprocess Quattro top layer (Q->top16) at full resolution */
    for (row = 0; row < qtop.rows; row++)
      for (col = 0; col < qtop.columns; col++) {
	uint16_t *valp = &qtop.data[qtop.row_stride*row + qtop.channels*col];
	int32_t out = (int32_t)round(scale[2] * (*valp - black_level[2]) +
				     ilevels->black[2]);

	if (out < 0) *valp = 0;
	else if (out > 65535) *valp = 65535;
	else *valp = out;
      }
    if (fix_bad) interpolate_bad_pixels(x3f, &qtop, 1, scale,
					intermediate_bias);
  }

  if (fix_bad) interpolate_bad_pixels(x3f, &image, 3, scale,
				      intermediate_bias);
  if (fix_bad && !quattro) repair_merrill_bad_clusters(x3f, &image);

  return 1;
}

/*
 * Merrill Photo Pro applies a white-balance-specific radial color-shading
 * correction immediately after its gain stage.  The four CAMF coefficients
 * describe two quartic-in-radius curves: one for the bottom (B) plane and one
 * for the top (T) plane.  The middle plane is unchanged.
 *
 * Reconstructed from FIPEngine2_F20::priProcessF20PreprocessStage in SPP 6.9.
 * Keeping this in the pre-matrix 14-bit domain also puts it before Merrill's
 * edge-aware denoising, matching the original ordering.
 */
static void apply_merrill_color_shading(x3f_t *x3f, x3f_area16_t *image,
					char *wb)
{
  double coeff[4];
  double center_x, center_y, denom_sq;
  uint32_t active[4], half_width, half_height, row, col;

  if (!x3f_get_camf_matrix_for_wb(x3f, "WhiteBalanceColorShadingFactor",
					  wb, 2, 2, coeff) ||
      !x3f_get_camf_rect(x3f, "ActiveImageArea", image, 1, active))
    return;

  half_width = (active[2] - active[0]) / 2;
  half_height = (active[3] - active[1]) / 2;
  center_x = active[0] + half_width;
  center_y = active[1] + half_height;
  denom_sq = (double)half_width * half_width +
    (double)half_height * half_height;
  if (denom_sq <= 0.0) return;

  x3f_printf(DEBUG, "Merrill color shading = {%g,%g,%g,%g}\n",
	     coeff[0], coeff[1], coeff[2], coeff[3]);
  for (row = 0; row < image->rows; ++row) {
    double dy = row - center_y;
    for (col = 0; col < image->columns; ++col) {
      double dx = col - center_x;
      double radius2 = (dx * dx + dy * dy) / denom_sq;
      double radius4 = radius2 * radius2;
      double bottom_gain = 1.0 + coeff[1] * radius2 + coeff[0] * radius4;
      double top_gain = 1.0 + coeff[3] * radius2 + coeff[2] * radius4;
      uint16_t *pixel = image->data + image->row_stride * row +
	image->channels * col;
      long bottom = lrint(pixel[0] * bottom_gain);
      long top = lrint(pixel[2] * top_gain);

      pixel[0] = (uint16_t)(bottom < 0 ? 0 : bottom > 65535 ? 65535 : bottom);
      pixel[2] = (uint16_t)(top < 0 ? 0 : top > 65535 ? 65535 : top);
    }
  }
}

/*
 * Expand Photo Pro's compact F20 saturation map.  Each pair is a delta from
 * the previous run end followed by a run length, in row-major pixel order.
 * The three maps are stored as separate CAMF matrices but Photo Pro expands
 * them into an interleaved byte triplet.
 */
static int decode_merrill_satmap(x3f_t *x3f, const char *name,
				 uint8_t *map, size_t pixels, int color,
				 size_t *marked)
{
  uint32_t *runs;
  uint64_t previous_end = 0;
  int count, index;
  size_t marked_count = 0;

  if (!x3f_get_camf_matrix_var(x3f, (char *)name, &count, NULL, NULL,
				M_UINT, (void **)&runs))
    return 0;

  for (index = 0; index + 1 < count; index += 2) {
    uint64_t start = previous_end + runs[index];
    uint64_t end = start + runs[index + 1];
    uint64_t pixel;

    if (start >= pixels) {
      if (start != pixels || end != pixels)
	x3f_printf(WARN, "%s run starts outside image (%llu >= %llu)\n",
		   name, (unsigned long long)start,
		   (unsigned long long)pixels);
      break;
    }
    if (end > pixels) {
      x3f_printf(WARN, "%s run clipped to image (%llu > %llu)\n",
		 name, (unsigned long long)end,
		 (unsigned long long)pixels);
      end = pixels;
    }
    for (pixel = start; pixel < end; ++pixel)
      map[3 * pixel + color] = 1;
    marked_count += (size_t)(end - start);
    previous_end = end;
  }
  if (marked) *marked = marked_count;
  return 1;
}

static double merrill_highlight_threshold(x3f_t *x3f)
{
  double estimate = 0.0, net = 0.0;
  double exposure, blend;

  x3f_get_camf_float(x3f, "ExpEstimate", &estimate);
  x3f_get_camf_float(x3f, "ExpNet", &net);
  exposure = estimate + net - 1.5;
  if (exposure < 0.0) exposure = 0.0;
  blend = exposure <= 1.0 ? 1.0 - exposure : 0.0;
  return blend * 3000.0 + 4000.0;
}

typedef struct {
  int enabled;
  double inverse_cc[9];
  double code_scale;
} merrill_sigma_hn_t;

/*
 * Photo Pro runs a second, later highlight-neutralization pass in Stage 3.
 * Sigma_HN measures the pixel after undoing the color-correction matrix, then
 * fades all three matrixed channels toward a common weighted sensor value as
 * the top (blue) Foveon plane crosses 2500..3450 codes.
 */
static void prepare_merrill_sigma_hn(x3f_t *x3f, double *conv_matrix,
				     merrill_sigma_hn_t *hn)
{
  double gain[3], cc[9], max_gain;
  int row, col;

  memset(hn, 0, sizeof(*hn));
  if (!x3f_get_gain(x3f, x3f_get_wb(x3f), gain)) return;
  max_gain = fmax(gain[0], fmax(gain[1], gain[2]));
  if (max_gain <= 0.0) return;

  /* conv = CC * diag(gain).  Native Stage 3 receives inverse(CC). */
  for (row = 0; row < 3; ++row)
    for (col = 0; col < 3; ++col)
      cc[3 * row + col] = conv_matrix[3 * row + col] / gain[col];
  x3f_3x3_inverse(cc, hn->inverse_cc);
  hn->code_scale = INTERMEDIATE_UNIT / max_gain;
  hn->enabled = 1;
  x3f_printf(DEBUG, "Merrill Sigma_HN code scale = %g\n", hn->code_scale);
}

static void apply_merrill_sigma_hn(const merrill_sigma_hn_t *hn, double *rgb)
{
  const double sensor_weights[3] = {0.03125, 0.03125, 0.9375};
  const double neutral_weights[3] = {0.25, 0.5, 0.25};
  double code_rgb[3], sensor[3], signal = 0.0, neutral = 0.0, amount;
  int color;

  if (!hn->enabled) return;
  for (color = 0; color < 3; ++color)
    code_rgb[color] = rgb[color] * hn->code_scale;
  x3f_3x3_3x1_mul((double *)hn->inverse_cc, code_rgb, sensor);
  for (color = 0; color < 3; ++color) {
    signal += sensor_weights[color] * sensor[color];
    neutral += neutral_weights[color] * sensor[color];
  }
  amount = (signal - 2500.0) / 950.0;
  if (amount <= 0.0) return;
  if (amount > 1.0) amount = 1.0;
  for (color = 0; color < 3; ++color)
    rgb[color] = ((1.0 - amount) * code_rgb[color] + amount * neutral) /
      hn->code_scale;
}

/*
 * Merrill highlight restoration reconstructed from
 * FPixelProc2_F20::F20RestoreHighlights in Sigma Photo Pro 6.9.
 *
 * This is deliberately before Darksub/despeckle and the camera matrix.  It
 * uses the camera-authored saturation maps to reconstruct a clipped top plane
 * from an image-specific B/T ratio curve, then converges saturated clusters
 * and their immediate neighborhood toward the strongest sensor plane.
 */
static void apply_merrill_highlight_restoration(x3f_t *x3f,
						x3f_area16_t *image)
{
  uint8_t *satmap = NULL, *neighborhood = NULL;
  uint32_t active[4];
  float ratio_sum[256] = {0.0f};
  float ratio_curve[256] = {0.0f};
  uint32_t ratio_count[256] = {0};
  size_t pixels, marked[3] = {0, 0, 0}, restored = 0, neutralized = 0;
  double threshold;
  int color, row, col, first_bin = -1;

  if (image->channels < 3 || image->columns < 5 || image->rows < 5 ||
      !x3f_get_camf_rect(x3f, "ActiveImageArea", image, 1, active))
    return;
  pixels = (size_t)image->columns * image->rows;
  if (pixels > SIZE_MAX / 3) return;
  satmap = calloc(pixels, 3);
  neighborhood = calloc(pixels, 1);
  if (!satmap || !neighborhood) goto cleanup;

  if (!decode_merrill_satmap(x3f, "SatMapR", satmap, pixels, 0,
			     &marked[0]) ||
      !decode_merrill_satmap(x3f, "SatMapG", satmap, pixels, 1,
			     &marked[1]) ||
      !decode_merrill_satmap(x3f, "SatMapB", satmap, pixels, 2,
			     &marked[2])) {
    x3f_printf(WARN, "Merrill compact saturation maps unavailable; "
	       "highlight restoration skipped\n");
    goto cleanup;
  }

  /* Photo Pro clears isolated flags in place, retaining only flags with an
     eight-connected neighbor in the same sensor plane. */
  for (row = 1; row + 1 < (int)image->rows; ++row) {
    for (col = 1; col + 1 < (int)image->columns; ++col) {
      size_t pixel = (size_t)row * image->columns + col;
      for (color = 0; color < 3; ++color) {
        uint8_t keep = 0;
        int dr, dc;
        if (!satmap[3 * pixel + color]) continue;
        for (dr = -1; dr <= 1 && !keep; ++dr)
          for (dc = -1; dc <= 1; ++dc) {
            size_t neighbor;
            if (!dr && !dc) continue;
            neighbor = (size_t)(row + dr) * image->columns + col + dc;
            if (satmap[3 * neighbor + color]) {
              keep = 1;
              break;
            }
          }
        satmap[3 * pixel + color] = keep;
      }
    }
  }

  /* A pixel clipped in at least two planes seeds a 5x5 support mask. */
  for (row = 2; row + 2 < (int)image->rows; ++row)
    for (col = 2; col + 2 < (int)image->columns; ++col) {
      size_t pixel = (size_t)row * image->columns + col;
      int dr, dc;
      if (satmap[3 * pixel] + satmap[3 * pixel + 1] +
	  satmap[3 * pixel + 2] < 2)
	continue;
      for (dr = -2; dr <= 2; ++dr)
	for (dc = -2; dc <= 2; ++dc)
	  neighborhood[(size_t)(row + dr) * image->columns + col + dc] = 1;
    }

  /* Learn top/middle from unsaturated bright pixels in this exposure. */
  for (row = (int)active[1]; row <= (int)active[3]; ++row)
    for (col = (int)active[0]; col <= (int)active[2]; ++col) {
      size_t pixel = (size_t)row * image->columns + col;
      uint16_t *value = image->data + image->row_stride * row +
	image->channels * col;
      float bottom, middle, top;
      int bin;
      if (satmap[3 * pixel + 2] || value[0] + value[1] <= 4000)
	continue;
      bottom = value[0] < 1 ? 1.0f : value[0];
      middle = value[1] < 1 ? 1.0f : value[1];
      top = value[2] < 1 ? 1.0f : value[2];
      bin = (int)(bottom * 255.0f / (bottom + middle));
      if (bin < 0) bin = 0;
      if (bin > 255) bin = 255;
      ratio_sum[bin] += top / middle;
      ++ratio_count[bin];
    }
  for (color = 0; color < 256; ++color) {
    if (!ratio_count[color]) continue;
    ratio_curve[color] = ratio_sum[color] / ratio_count[color];
    if (first_bin < 0) first_bin = color;
  }
  if (first_bin >= 0) {
    float previous = ratio_curve[first_bin];
    for (color = 0; color < 256; ++color) {
      if (ratio_curve[color] == 0.0f)
	ratio_curve[color] = previous;
      else
	previous = ratio_curve[color];
    }
  }

  threshold = merrill_highlight_threshold(x3f);
  for (row = (int)active[1] > 2 ? (int)active[1] : 2;
       row <= (int)active[3] && row + 2 < (int)image->rows; ++row) {
    for (col = (int)active[0] > 2 ? (int)active[0] : 2;
	 col <= (int)active[2] && col + 2 < (int)image->columns; ++col) {
      size_t pixel = (size_t)row * image->columns + col;
      uint16_t *value = image->data + image->row_stride * row +
	image->channels * col;
      double planes[3], maximum, middle, minimum, signal, support, amount;
      int dr, dc;

      planes[0] = value[0];
      planes[1] = value[1];
      planes[2] = value[2];
      if (first_bin >= 0 && satmap[3 * pixel + 2] &&
	  planes[0] + planes[1] > 0.0) {
	int bin = (int)(planes[0] * 255.0 / (planes[0] + planes[1]));
	double predicted;
	if (bin < 0) bin = 0;
	if (bin > 255) bin = 255;
	predicted = planes[1] * ratio_curve[bin];
	planes[2] = predicted > 32767.0 ? 32767.0 : predicted;
	++restored;
      }

      maximum = fmax(planes[0], fmax(planes[1], planes[2]));
      minimum = fmin(planes[0], fmin(planes[1], planes[2]));
      middle = planes[0] + planes[1] + planes[2] - maximum - minimum;
      signal = ((2.0 / 3.0) * maximum + (1.0 / 3.0) * middle -
		threshold) / 1000.0;
      if (signal < 0.0) signal = 0.0;
      if (signal > 1.0) signal = 1.0;
      support = 0.0;
      for (dr = -2; dr <= 2; ++dr)
	for (dc = -2; dc <= 2; ++dc)
	  support += neighborhood[(size_t)(row + dr) * image->columns + col + dc];
      support *= 0.04;
      amount = sqrt(0.5 * (signal * signal + support * support));
      if (amount <= 0.0) continue;
      if (amount > 1.0) amount = 1.0;
      for (color = 0; color < 3; ++color) {
	/* RepairPix uses the first (deep/red) sensor plane as the neutral
	   anchor; the sorted maximum above is used only for signal strength. */
	double output = amount * planes[0] + (1.0 - amount) * planes[color];
	if (output < 0.0) output = 0.0;
	if (output > 32767.0) output = 32767.0;
	value[color] = (uint16_t)lrint(output);
      }
      ++neutralized;
    }
  }
  x3f_printf(DEBUG,
	     "Merrill F20 highlights: image=%ux%u active={%u,%u,%u,%u} "
	     "sat={%llu,%llu,%llu} curve_bins=%d threshold=%g "
	     "restored=%llu neutralized=%llu\n",
	     image->columns, image->rows, active[0], active[1], active[2], active[3],
	     (unsigned long long)marked[0], (unsigned long long)marked[1],
	     (unsigned long long)marked[2], first_bin < 0 ? 0 : 256 - first_bin,
	     threshold, (unsigned long long)restored,
	     (unsigned long long)neutralized);

cleanup:
  free(neighborhood);
  free(satmap);
}

/*
 * Merrill's "Normal Despeckle" is a sparse sensor-defect pass, not a spatial
 * denoiser.  DetectSpecklesRas tests every B/M/T plane independently.  A
 * sample is recorded only when it is a strict peak or valley against all
 * eight neighbors and clears the plane's signal-dependent threshold.  The
 * combined channel mask is then sent through ReplaceBadPixelsF20 mode 3.
 *
 * Photo Pro performs this on its floating-point, gain-scaled preprocess
 * image.  native_per_intermediate converts the biased 14-bit x3f-tools image
 * back to those units; the repair itself stays in the integer representation
 * because the recovered interpolation formulas are affine-invariant.
 *
 * Reconstructed from FPixelProc2_F20::DespeckleRas at 0x4a62c and
 * DetectSpecklesRas at 0x49ac8 in the Merrill processing library.
 */
static void apply_merrill_rgb_despeckle(x3f_t *x3f, x3f_area16_t *image,
					char *wb,
					double intermediate_bias,
					const double *native_per_intermediate)
{
  static const double noise_scale[3] = {1.1e-14, 7.5e-15, 3.2e-15};
  static const double noise_floor[3] = {6.4e-7, 8.1e-7, 4.0e-8};
  int32_t adjust_i[3] = {10, 10, 5};
  double gain[3], strength = 1.0;
  double *iso_settings = NULL, capture_iso;
  int iso_rows, iso_columns;
  float *threshold = NULL;
  bad_pixel_t *speckles = NULL;
  size_t count = 0, capacity = 0;
  size_t peaks[3] = {0, 0, 0}, valleys[3] = {0, 0, 0};
  uint32_t active[4];
  unsigned int row_begin, row_end, col_begin, col_end, row, col, channel;
  const char *setting;

  if (!image || image->channels < 3 || image->rows < 3 || image->columns < 3 ||
      !native_per_intermediate || getenv("FAST_SIGMA_DISABLE_NATIVE_DESPECKLE") ||
      !x3f_get_gain(x3f, wb, gain))
    return;
  for (channel = 0; channel < 3; ++channel)
    if (gain[channel] <= 0.0 || native_per_intermediate[channel] <= 0.0)
      return;

  /* The Merrill CAMF stores this as a signed 16-bit vector. */
  x3f_get_camf_signed_vector(x3f, "DespAdjust", adjust_i);

  /* SetUpNoiseParamsF20 writes element 1 of the ISO-interpolated 13-value
     ISONoiseSettings row to NoiseParams+20.  The preprocess stage multiplies
     every DespAdjust component by that value before calling DespeckleRas.
     DP Merrill CAMFs use 1.0 at every ISO, but read the native table instead
     of baking that camera-specific fact into the detector. */
  if (x3f_get_camf_matrix_var(x3f, "ISONoiseSettings", &iso_rows,
			      &iso_columns, NULL, M_FLOAT,
			      (void **)&iso_settings) &&
      iso_rows > 0 && iso_columns == 13 &&
      x3f_get_camf_float(x3f, "CaptureISO", &capture_iso)) {
    int high = 0;
    while (high < iso_rows - 1 &&
	   capture_iso > iso_settings[13 * high])
      ++high;
    if (high == 0) {
      strength = iso_settings[1];
    } else {
      double low_iso = iso_settings[13 * (high - 1)];
      double high_iso = iso_settings[13 * high];
      double fraction = high_iso > low_iso
	? (capture_iso - low_iso) / (high_iso - low_iso) : 0.0;
      if (fraction < 0.0) fraction = 0.0;
      if (fraction > 1.0) fraction = 1.0;
      strength = iso_settings[13 * (high - 1) + 1] * (1.0 - fraction) +
	iso_settings[13 * high + 1] * fraction;
    }
  }
  setting = getenv("FAST_SIGMA_DESPECKLE_STRENGTH");
  if (setting) strength = strtod(setting, NULL);
  if (strength <= 0.0) return;

  threshold = malloc(3 * 4096 * sizeof(*threshold));
  if (!threshold) return;
  for (channel = 0; channel < 3; ++channel) {
    double variance_scale = noise_scale[channel] / gain[channel];
    double amount = adjust_i[channel] * strength;
    unsigned int code;
    for (code = 0; code < 4096; ++code) {
      double normalized = sqrt(code * 0.000341796875 * variance_scale /
			       1.392e-19) * 1.6e-19 / variance_scale;
      double native_threshold = sqrt(noise_floor[channel] +
				     0.7569 * normalized * normalized) *
	amount * 4096.0 / 1.4;
      threshold[4096 * channel + code] =
	(float)(native_threshold / native_per_intermediate[channel]);
    }
  }

  /* Sigma expands the active rectangle by four pixels and uses the active
     top/left margins symmetrically at the far edge. */
  if (x3f_get_camf_rect(x3f, "ActiveImageArea", image, 1, active)) {
    unsigned int top_margin = active[1] > 4 ? active[1] - 4 : 1;
    unsigned int left_margin = active[0] > 4 ? active[0] - 4 : 1;
    row_begin = top_margin > 1 ? top_margin : 1;
    col_begin = left_margin > 1 ? left_margin : 1;
    row_end = image->rows > top_margin ? image->rows - top_margin : image->rows - 1;
    col_end = image->columns > left_margin ? image->columns - left_margin : image->columns - 1;
  } else {
    row_begin = col_begin = 1;
    row_end = image->rows - 1;
    col_end = image->columns - 1;
  }
  if (row_end > image->rows - 1) row_end = image->rows - 1;
  if (col_end > image->columns - 1) col_end = image->columns - 1;

  for (row = row_begin; row < row_end; ++row) {
    for (col = col_begin; col < col_end; ++col) {
      const uint16_t *center = image->data + image->row_stride * row +
	image->channels * col;
      unsigned int channel_mask = 0;
      for (channel = 0; channel < 3; ++channel) {
	uint16_t low = 65535, high = 0;
	double native_center;
	unsigned int code;
	int dr, dc;
	for (dr = -1; dr <= 1; ++dr)
	  for (dc = -1; dc <= 1; ++dc) {
	    const uint16_t *neighbor;
	    uint16_t value;
	    if (!dr && !dc) continue;
	    neighbor = image->data + image->row_stride * (row + dr) +
	      image->channels * (col + dc);
	    value = neighbor[channel];
	    if (value < low) low = value;
	    if (value > high) high = value;
	  }
	if (center[channel] <= high && center[channel] >= low) continue;
	native_center = (center[channel] - intermediate_bias) *
	  native_per_intermediate[channel];
	if (native_center < 0.0) native_center = 0.0;
	if (native_center > 4095.0) native_center = 4095.0;
	code = (unsigned int)native_center;
	if (center[channel] > high + threshold[4096 * channel + code]) {
	  channel_mask |= 1u << (2 - channel);
	  ++peaks[channel];
	} else if (center[channel] + threshold[4096 * channel + code] < low) {
	  channel_mask |= 1u << (2 - channel);
	  ++valleys[channel];
	}
      }
      if (!channel_mask) continue;
      if (count == capacity) {
	size_t new_capacity = capacity ? 2 * capacity : 4096;
	bad_pixel_t *grown = realloc(speckles, new_capacity * sizeof(*grown));
	if (!grown) goto repair;
	speckles = grown;
	capacity = new_capacity;
      }
      speckles[count].c = col;
      speckles[count].r = row;
      speckles[count].f20_flags = (uint16_t)((channel_mask << 12) | 0x3ff);
      speckles[count].prev = speckles[count].next = NULL;
      ++count;
    }
  }

repair:
  for (row = 0; row < count; ++row)
    repair_f20_pixel(image, &speckles[row]);
  x3f_printf(DEBUG,
    "Merrill RGB despeckle: adjust={%d,%d,%d} strength=%g found=%llu "
    "peaks={%llu,%llu,%llu} valleys={%llu,%llu,%llu}\n",
    adjust_i[0], adjust_i[1], adjust_i[2], strength,
    (unsigned long long)count,
    (unsigned long long)peaks[0], (unsigned long long)peaks[1],
    (unsigned long long)peaks[2], (unsigned long long)valleys[0],
    (unsigned long long)valleys[1], (unsigned long long)valleys[2]);
  free(speckles);
  free(threshold);
}

static int get_conv(x3f_t *x3f, x3f_color_encoding_t encoding, char *wb,
		    int lutsize, uint16_t max_out, double *lut,
		    double *conv_matrix)
{
  double raw_to_xyz[9];	/* White point for XYZ is assumed to be D65 */
  double xyz_to_rgb[9];
  double raw_to_rgb[9];
  double sensor_iso, capture_iso, iso_scaling;

  if (x3f_get_camf_float(x3f, "SensorISO", &sensor_iso) &&
      x3f_get_camf_float(x3f, "CaptureISO", &capture_iso)) {
    x3f_printf(DEBUG, "SensorISO = %g\n", sensor_iso);
    x3f_printf(DEBUG, "CaptureISO = %g\n", capture_iso);
    iso_scaling = capture_iso/sensor_iso;
  }
  else {
    iso_scaling = 1.0;
    x3f_printf(WARN, "Could not calculate ISO scaling, assuming %g\n",
	       iso_scaling);
  }

  if (!x3f_get_raw_to_xyz(x3f, wb, raw_to_xyz)) {
    x3f_printf(ERR, "Could not get raw_to_xyz for white balance: %s\n", wb);
    return 0;
  }

  switch (encoding) {
  case LINEAR_SRGB:
    x3f_gamma_LUT(lut, lutsize, max_out, 1.0);
    x3f_XYZ_to_sRGB(xyz_to_rgb);
    break;
  case LINEAR_ROMM:
    {
      double xyz_to_romm[9], d65_to_d50[9];
      x3f_gamma_LUT(lut, lutsize, max_out, 1.0);
      x3f_XYZ_to_ProPhotoRGB(xyz_to_romm);
      x3f_Bradford_D65_to_D50(d65_to_d50);
      x3f_3x3_3x3_mul(xyz_to_romm, d65_to_d50, xyz_to_rgb);
    }
    break;
  case SRGB:
    x3f_sRGB_LUT(lut, lutsize, max_out);
    x3f_XYZ_to_sRGB(xyz_to_rgb);
    break;
  case ARGB:
    x3f_gamma_LUT(lut, lutsize, max_out, 2.2);
    x3f_XYZ_to_AdobeRGB(xyz_to_rgb);
    break;
  case PPRGB:
    {
      double xyz_to_prophotorgb[9], d65_to_d50[9];

      x3f_gamma_LUT(lut, lutsize, max_out, 1.8);
      x3f_XYZ_to_ProPhotoRGB(xyz_to_prophotorgb);
      /* The standad white point for ProPhoto RGB is D50 */
      x3f_Bradford_D65_to_D50(d65_to_d50);
      x3f_3x3_3x3_mul(xyz_to_prophotorgb, d65_to_d50, xyz_to_rgb);
    }
    break;
  default:
    x3f_printf(ERR, "Unknown color space %d\n", encoding);
    return 0;
  }

  x3f_3x3_3x3_mul(xyz_to_rgb, raw_to_xyz, raw_to_rgb);
  x3f_scalar_3x3_mul(iso_scaling, raw_to_rgb, conv_matrix);

  x3f_printf(DEBUG, "raw_to_rgb\n");
  x3f_3x3_print(DEBUG, raw_to_rgb);
  x3f_printf(DEBUG, "conv_matrix\n");
  x3f_3x3_print(DEBUG, conv_matrix);

  return 1;
}

typedef struct {
  int16_t *channel[3];
  uint32_t length;
  uint32_t mask;
} color_dq_lut_t;

/*
 * Merrill's ColorDQ stage, independently reconstructed from Photo Pro 6.9's
 * FPixelProc2_F20::GetTanhLUT and ApplyMatrixAndColorDQ routines.  It is a
 * small, symmetric set of three one-dimensional chroma-residual curves, not
 * a 3D color lookup table.  Values are expressed in the F20 14-bit domain.
 */
static int create_merrill_color_dq_lut(x3f_t *x3f, color_dq_lut_t *lut)
{
  static const double slope[3] = {0.8, 0.8, 0.8};
  double amount[3], max_ratio, step;
  uint32_t requested, half, i, c;

  memset(lut, 0, sizeof(*lut));
  if (!x3f_get_camf_float_vector(x3f, "ColorDQCamRGB", amount)) return 0;
  if (amount[0] <= 0.0 || amount[1] <= 0.0 || amount[2] <= 0.0) return 0;

  max_ratio = amount[0] / slope[0];
  for (c = 1; c < 3; ++c)
    if (amount[c] / slope[c] > max_ratio) max_ratio = amount[c] / slope[c];
  step = 0.25 / max_ratio;
  requested = (uint32_t)(6.2853 / step);
  if (requested < 513) lut->length = 512;
  else if (requested < 1025) lut->length = 1024;
  else if (requested < 2049) lut->length = 2048;
  else if (requested > 4096) lut->length = 8192;
  else lut->length = 4096;
  lut->mask = lut->length - 1;

  for (c = 0; c < 3; ++c) {
    lut->channel[c] = calloc(lut->length, sizeof(int16_t));
    if (!lut->channel[c]) goto fail;
  }

  half = lut->length / 2;
  for (i = 0; i < half; ++i) {
    double phase = step * i;
    double window = phase < 3.1416 ? 0.5 * (cos(phase) + 1.0) : 0.0;
    for (c = 0; c < 3; ++c) {
      int value = (int)(amount[c] * window *
                        tanh(slope[c] * i / amount[c]) + 0.5);
      lut->channel[c][i] = (int16_t)value;
      if (i != 0) lut->channel[c][(-i) & lut->mask] = (int16_t)-value;
    }
  }
  return 1;

fail:
  for (c = 0; c < 3; ++c) free(lut->channel[c]);
  memset(lut, 0, sizeof(*lut));
  return 0;
}

static void cleanup_color_dq_lut(color_dq_lut_t *lut)
{
  uint32_t c;
  for (c = 0; c < 3; ++c) free(lut->channel[c]);
  memset(lut, 0, sizeof(*lut));
}

static void apply_merrill_color_dq(const color_dq_lut_t *lut, double *rgb)
{
  const double f20_scale = 16383.0;
  int32_t code[3], residual[3], gray;
  uint32_t c;

  for (c = 0; c < 3; ++c) {
    code[c] = (int32_t)lrint(rgb[c] * f20_scale);
    residual[c] = code[c] - lut->channel[c][code[c] & lut->mask];
  }
  gray = (residual[0] + 2 * residual[1] + residual[2]) >> 2;
  for (c = 0; c < 3; ++c) {
    int32_t chroma = residual[c] - gray;
    rgb[c] -= lut->channel[c][chroma & lut->mask] / f20_scale;
  }
}

/* Legacy empirical sRGB shadow correction retained only for differential
   testing. Production Merrill DNGs preserve Sigma's linear ROMM working space
   and never call this unless FAST_SIGMA_ENABLE_LEGACY_SHADOW_CORRECTION is
   explicitly set. */
static void correct_merrill_shadow_green(double *rgb)
{
  const double shadow_end = 0.02;
  const double midtone_start = 0.20;
  const double green_to_red = 0.90;
  const double green_to_blue = 1.0 - green_to_red;
  double corrected[3] = {
    rgb[0] > 0.0 ? rgb[0] : 0.0,
    rgb[1] > 0.0 ? rgb[1] : 0.0,
    rgb[2] > 0.0 ? rgb[2] : 0.0,
  };
  double luminance =
    0.2126 * corrected[0] + 0.7152 * corrected[1] + 0.0722 * corrected[2];
  double corrected_luminance, green_excess, green_fraction;
  double transfer, weight, position, scale;
  int clipped_chroma = rgb[0] <= 0.0 || rgb[2] <= 0.0;

  if (luminance <= 0.0 || luminance >= midtone_start) return;

  if (luminance <= shadow_end) {
    weight = 1.0;
  } else {
    position = (luminance - shadow_end) / (midtone_start - shadow_end);
    weight = 1.0 - position * position * (3.0 - 2.0 * position);
  }

  /*
   * If a signed camera-matrix result has lost red and blue below zero, gains
   * alone cannot repair the remaining green spike.  Move that excess mostly
   * toward red (the amber/magenta direction used in Photo Pro), with a small
   * blue component so near-black neutrals do not become pure red.  This term
   * naturally vanishes for real colors whose green is not the sole outlier.
  */
  green_excess = corrected[1] - fmax(corrected[0], corrected[2]);
  if (clipped_chroma && green_excess > 0.0) {
    green_fraction = green_excess / corrected[1];
    transfer = weight * green_excess;
    corrected[0] += green_to_red * transfer;
    corrected[2] += green_to_blue * transfer;
    corrected[1] -= transfer *
      (0.2126 * green_to_red + 0.0722 * green_to_blue) / 0.7152;
  } else {
    green_fraction = 0.0;
  }

  /* Paired neutral pixels retain a smaller residual warm-shadow difference. */
  corrected[0] *= exp2(0.16 * weight * (1.0 - green_fraction));
  corrected[2] *= exp2(-0.25 * weight * (1.0 - green_fraction));

  corrected_luminance =
    0.2126 * corrected[0] +
    0.7152 * corrected[1] +
    0.0722 * corrected[2];
  if (corrected_luminance <= 0.0) return;
  scale = luminance / corrected_luminance;
  rgb[0] = corrected[0] * scale;
  rgb[1] = corrected[1] * scale;
  rgb[2] = corrected[2] * scale;
}

/* Converts the data in place */

#define LUTSIZE 1024

static int convert_data(x3f_t *x3f,
			x3f_area16_t *image, x3f_image_levels_t *ilevels,
			x3f_color_encoding_t encoding,
			int apply_sgain,
			char *wb)
{
  int row, col, color;
  uint16_t max_out = 65535; /* TODO: should be possible to adjust */

  double conv_matrix[9];
  double lut[LUTSIZE];
  x3f_spatial_gain_corr_t sgain[MAXCORR];
  int sgain_num;
  color_dq_lut_t color_dq;
  int apply_color_dq = 0;
  merrill_sigma_hn_t sigma_hn;

  if (image->channels < 3) return 0;

  if (encoding == LINEAR_BMT) {
    x3f_3x3_identity(conv_matrix);
    x3f_gamma_LUT(lut, LUTSIZE, max_out, 1.0);
  } else if (!get_conv(x3f, encoding, wb, LUTSIZE, max_out, lut,
			       conv_matrix)) {
    return 0;
  }

  memset(&color_dq, 0, sizeof(color_dq));
  memset(&sigma_hn, 0, sizeof(sigma_hn));
  if (encoding == LINEAR_SRGB || encoding == LINEAR_ROMM) {
    char *sensorid = NULL;
    apply_color_dq = x3f_get_prop_entry(x3f, "SENSORID", &sensorid) &&
      sensorid && !strcmp(sensorid, "F20") &&
      create_merrill_color_dq_lut(x3f, &color_dq);
    if (apply_color_dq && !getenv("FAST_SIGMA_DISABLE_NATIVE_HIGHLIGHT"))
      prepare_merrill_sigma_hn(x3f, conv_matrix, &sigma_hn);
  }

  if (apply_sgain) {
    sgain_num = x3f_get_spatial_gain(x3f, wb, sgain);
    if (sgain_num == 0)
      x3f_printf(WARN, "Could not get spatial gain\n");
  } else {
    sgain_num = 0;
  }

  for (row = 0; row < image->rows; row++) {
    for (col = 0; col < image->columns; col++) {
      uint16_t *valp[3];
      double input[3], output[3];

      /* Get the data */
      for (color = 0; color < 3; color++) {
	valp[color] =
	  &image->data[image->row_stride*row + image->channels*col + color];
	input[color] = x3f_calc_spatial_gain(sgain, sgain_num,
					     row, col, color,
					     image->rows, image->columns) *
	  (*valp[color] - ilevels->black[color]) /
	  (ilevels->white[color] - ilevels->black[color]);
      }

      /* Do color conversion */
      x3f_3x3_3x1_mul(conv_matrix, input, output);
      if (apply_color_dq) {
	apply_merrill_color_dq(&color_dq, output);
	/* Photo Pro matrices the local-mean base first and only then restores
	   the cleaned residual.  At Merrill ISO 100 this is a common RGB gain,
	   which preserves chroma through signed camera-matrix excursions. */
	for (color = 0; color < 3; ++color)
	  output[color] *= x3f_merrill_residual_gain_at(x3f, valp[0], color);
	apply_merrill_sigma_hn(&sigma_hn, output);
	if (getenv("FAST_SIGMA_ENABLE_LEGACY_SHADOW_CORRECTION"))
	  correct_merrill_shadow_green(output);
      }

      /* Write back the data, doing non linear coding */
      for (color = 0; color < 3; color++)
	*valp[color] = x3f_LUT_lookup(lut, LUTSIZE, output[color]);
    }
  }

  x3f_cleanup_spatial_gain(sgain, sgain_num);
  cleanup_color_dq_lut(&color_dq);

  ilevels->black[0] = ilevels->black[1] = ilevels->black[2] = 0.0;
  ilevels->white[0] = ilevels->white[1] = ilevels->white[2] = max_out;

  return 1;
}

/* extern */ int x3f_convert_image(x3f_t *x3f,
				    x3f_area16_t *image,
				    x3f_image_levels_t *ilevels,
				    x3f_color_encoding_t encoding,
				    int apply_sgain,
				    char *wb)
{
  if (wb == NULL) wb = x3f_get_wb(x3f);
  if (encoding == NONE || encoding == UNPROCESSED || encoding == QTOP)
    return 0;
  return convert_data(x3f, image, ilevels, encoding, apply_sgain, wb);
}

static float median5f(float a, float b, float c, float d, float e)
{
  float values[5] = {a, b, c, d, e};
  int i, j;
  for (i = 1; i < 5; ++i) {
    float value = values[i];
    for (j = i; j > 0 && values[j - 1] > value; --j)
      values[j] = values[j - 1];
    values[j] = value;
  }
  return values[2];
}

static void transform_merrill_row(const x3f_area16_t *image, uint32_t row,
				  const double *matrix, float *output)
{
  uint32_t col;
  for (col = 0; col < image->columns; ++col) {
    const uint16_t *pixel = image->data + image->row_stride * row +
      image->channels * col;
    double input[3] = {pixel[0], pixel[1], pixel[2]};
    double transformed[3];
    x3f_3x3_3x1_mul((double *)matrix, input, transformed);
    output[3 * col + 0] = transformed[0];
    output[3 * col + 1] = transformed[1];
    output[3 * col + 2] = transformed[2];
  }
}

/*
 * FPixelProc2_F20::DarksubDespeckle transforms the image into Photo Pro's
 * ROMM-like working space, replaces each interior channel with the median of
 * its center/left/right/up/down cross, and transforms back.  Row scaling in
 * CalculateCAM2RIMMMatrix does not change a per-channel median, so the
 * Standard-mode transform below can omit that normalization and its inverse.
 *
 * FAST_SIGMA_DISABLE_DARKSUB_DESPECKLE is retained only for differential
 * regression tests.
 */
static void apply_merrill_darksub_despeckle(x3f_t *x3f,
					    x3f_area16_t *image, char *wb)
{
  double bmt_to_xyz[9], d65_to_d50[9], xyz_to_romm[9];
  double adapted[9], working[9], inverse[9];
  float *rows[3], *temporary;
  size_t row_elements;
  uint32_t row, col, color;

  if (getenv("FAST_SIGMA_DISABLE_DARKSUB_DESPECKLE") || image->channels < 3 ||
      image->rows < 3 || image->columns < 3 ||
      !x3f_get_bmt_to_xyz(x3f, wb, bmt_to_xyz))
    return;

  x3f_Bradford_D65_to_D50(d65_to_d50);
  x3f_XYZ_to_ProPhotoRGB(xyz_to_romm);
  x3f_3x3_3x3_mul(d65_to_d50, bmt_to_xyz, adapted);
  x3f_3x3_3x3_mul(xyz_to_romm, adapted, working);
  x3f_3x3_inverse(working, inverse);

  row_elements = (size_t)image->columns * 3;
  rows[0] = malloc(row_elements * sizeof(*rows[0]));
  rows[1] = malloc(row_elements * sizeof(*rows[1]));
  rows[2] = malloc(row_elements * sizeof(*rows[2]));
  if (!rows[0] || !rows[1] || !rows[2]) goto done;

  transform_merrill_row(image, 0, working, rows[0]);
  transform_merrill_row(image, 1, working, rows[1]);
  transform_merrill_row(image, 2, working, rows[2]);
  for (row = 1; row + 1 < image->rows; ++row) {
    for (col = 1; col + 1 < image->columns; ++col) {
      double filtered[3], restored[3];
      uint16_t *pixel = image->data + image->row_stride * row +
	image->channels * col;
      for (color = 0; color < 3; ++color) {
	const size_t center = 3 * col + color;
	filtered[color] = median5f(rows[1][center],
				  rows[1][center - 3], rows[1][center + 3],
				  rows[0][center], rows[2][center]);
      }
      x3f_3x3_3x1_mul(inverse, filtered, restored);
      for (color = 0; color < 3; ++color) {
	long value = lrint(restored[color]);
	pixel[color] = (uint16_t)(value < 0 ? 0 : value > 65535 ? 65535 : value);
      }
    }
    if (row + 2 < image->rows) {
      temporary = rows[0];
      rows[0] = rows[1];
      rows[1] = rows[2];
      rows[2] = temporary;
      transform_merrill_row(image, row + 2, working, rows[2]);
    }
  }

done:
  free(rows[2]);
  free(rows[1]);
  free(rows[0]);
}

/*
 * Correlation row-offset suppression from Merrill's preprocessing stage.
 * Sigma estimates offsets at 31, 73 and 151-pixel support scales, rejects
 * blocks dominated by scene edges, requires at least twenty observations,
 * and clamps implausibly large corrections.  Its production implementation
 * fits small polynomials inside tiles; the equivalent float-domain estimator
 * below uses the same scales and gates but accumulates the robust vertical
 * correlation residual directly.  This avoids fitting scene gradients into
 * the sensor row correction and is deterministic across thread counts.
 */
static void apply_merrill_row_offset_correction(x3f_area16_t *image)
{
  static const unsigned int scales[3] = {31, 73, 151};
  double *offset = NULL;
  uint32_t row, col, channel;
  unsigned int scale_index;
  size_t corrected_rows = 0;

  if (getenv("FAST_SIGMA_DISABLE_ROW_OFFSET") || image->channels < 3 ||
      image->rows < 5 || image->columns < 151)
    return;
  offset = calloc((size_t)image->rows * 3, sizeof(*offset));
  if (!offset) return;

  for (row = 2; row + 2 < image->rows; ++row) {
    for (channel = 0; channel < 3; ++channel) {
      double scale_sum = 0.0;
      unsigned int used_scales = 0;
      for (scale_index = 0; scale_index < 3; ++scale_index) {
	unsigned int width = scales[scale_index];
	double block_sum = 0.0;
	unsigned int blocks = 0;
	for (col = 0; col + width <= image->columns; col += width) {
	  double sum = 0.0, sum_abs = 0.0;
	  unsigned int samples = 0, x;
	  for (x = col; x < col + width; ++x) {
	    const uint16_t *above = image->data +
	      (row - 1) * image->row_stride + x * image->channels;
	    const uint16_t *center = image->data +
	      row * image->row_stride + x * image->channels;
	    const uint16_t *below = image->data +
	      (row + 1) * image->row_stride + x * image->channels;
	    double da = (double)center[channel] - above[channel];
	    double db = (double)center[channel] - below[channel];
	    double residual;
	    /* Scene edges do not provide a reliable row-correlation sample. */
	    if (fabs(da) + fabs(db) > 384.0) continue;
	    residual = 0.5 * (da + db);
	    if (fabs(residual) > 96.0) continue;
	    sum += residual;
	    sum_abs += fabs(residual);
	    ++samples;
	  }
	  if (samples < width / 3) continue;
	  sum /= samples;
	  /* Reject incoherent blocks rather than averaging texture into CROC. */
	  if (fabs(sum) > 24.0 || sum_abs / samples > 20.0) continue;
	  block_sum += sum;
	  ++blocks;
	}
	if (blocks >= 20) {
	  scale_sum += block_sum / blocks;
	  ++used_scales;
	}
      }
      if (used_scales) {
	double estimate = 0.5 * scale_sum / used_scales;
	if (fabs(estimate) <= 12.0)
	  offset[3 * row + channel] = estimate;
      }
    }
  }

  for (row = 2; row + 2 < image->rows; ++row) {
    int any = 0;
    for (channel = 0; channel < 3; ++channel)
      if (offset[3 * row + channel] != 0.0) any = 1;
    if (!any) continue;
    ++corrected_rows;
    for (col = 0; col < image->columns; ++col) {
      uint16_t *pixel = image->data + row * image->row_stride +
	col * image->channels;
      for (channel = 0; channel < 3; ++channel) {
	long value = lrint((double)pixel[channel] -
			   offset[3 * row + channel]);
	pixel[channel] = (uint16_t)(value < 0 ? 0 :
				    value > 65535 ? 65535 : value);
      }
    }
  }
  x3f_printf(DEBUG, "Merrill CROC corrected %zu rows at 31/73/151 scales\n",
	     corrected_rows);
  free(offset);
}

static int run_denoising(x3f_t *x3f)
{
  x3f_area16_t original_image, image;
  x3f_denoise_type_t type = X3F_DENOISE_STD;
  char *sensorid;

  if (!x3f_image_area(x3f, &original_image)) return 0;
  if (!x3f_crop_area_camf(x3f, "ActiveImageArea", &original_image, 1, &image)) {
    image = original_image;
    x3f_printf(WARN, "Could not get active area, denoising entire image\n");
  }

  if (x3f_get_prop_entry(x3f, "SENSORID", &sensorid) &&
      !strcmp(sensorid, "F20")) {
    type = X3F_DENOISE_F20;
    apply_merrill_darksub_despeckle(x3f, &image, x3f_get_wb(x3f));
    apply_merrill_row_offset_correction(&image);
  }

  x3f_denoise(x3f, &image, type);
  return 1;
}

static int expand_quattro(x3f_t *x3f, int denoise, x3f_area16_t *expanded)
{
  x3f_area16_t image, active, qtop, qtop_crop, active_exp;
  uint32_t rect[4];

  if (!x3f_image_area_qtop(x3f, &qtop)) return 0;
  if (!x3f_image_area(x3f, &image)) return 0;
  if (denoise &&
      !x3f_crop_area_camf(x3f, "ActiveImageArea", &image, 1, &active)) {
    active = image;
    x3f_printf(WARN, "Could not get active area, denoising entire image\n");
  }

  rect[0] = 0;
  rect[1] = 0;
  rect[2] = 2*image.columns - 1;
  rect[3] = 2*image.rows - 1;
  if (!x3f_crop_area(rect, &qtop, &qtop_crop)) return 0;

  expanded->columns = qtop_crop.columns;
  expanded->rows = qtop_crop.rows;
  expanded->channels = 3;
  expanded->row_stride = expanded->columns*expanded->channels;
  expanded->data = expanded->buf =
    malloc(expanded->rows*expanded->row_stride*sizeof(uint16_t));

  if (denoise && !x3f_crop_area_camf(x3f, "ActiveImageArea", expanded, 0,
				     &active_exp)) {
    active_exp = *expanded;
    x3f_printf(WARN, "Could not get active area, denoising entire image\n");
  }

  x3f_expand_quattro(&image, denoise ? &active : NULL, &qtop_crop,
		     expanded, denoise ? &active_exp : NULL);

  return 1;
}

/* extern */ int x3f_get_image(x3f_t *x3f,
			       x3f_area16_t *image,
			       x3f_image_levels_t *ilevels,
			       x3f_color_encoding_t encoding,
			       int crop,
			       int fix_bad,
			       int denoise,
			       int apply_sgain,
			       char *wb)
{
  x3f_area16_t original_image, expanded;
  x3f_image_levels_t il;
  double native_per_intermediate[3] = {0.0, 0.0, 0.0};

  if (wb == NULL) wb = x3f_get_wb(x3f);

  if (encoding == QTOP) {
    x3f_area16_t qtop;

    if (!x3f_image_area_qtop(x3f, &qtop)) return 0;
    if (!crop || !x3f_crop_area_camf(x3f, "ActiveImageArea", &qtop, 0, image))
      *image = qtop;

    return ilevels == NULL;
  }

  if (!x3f_image_area(x3f, &original_image)) return 0;
  if (!crop || !x3f_crop_area_camf(x3f, "ActiveImageArea", &original_image, 1,
				   image))
    *image = original_image;

  if (encoding == UNPROCESSED) return ilevels == NULL;

  if (!preprocess_data(x3f, fix_bad, wb, &il,
		       native_per_intermediate)) return 0;

  if (denoise) {
    char *sensorid = NULL;
    if (x3f_get_prop_entry(x3f, "SENSORID", &sensorid) && sensorid &&
	!strcmp(sensorid, "F20")) {
      apply_merrill_color_shading(x3f, &original_image, wb);
      if (!getenv("FAST_SIGMA_DISABLE_NATIVE_HIGHLIGHT"))
	apply_merrill_highlight_restoration(x3f, &original_image);
	apply_merrill_rgb_despeckle(x3f, &original_image, wb, il.black[0],
				  native_per_intermediate);
    }
  }

  if (expand_quattro(x3f, denoise, &expanded)) {
    /* NOTE: expand_quattro destroys the data of original_image */
    if (!crop ||
	!x3f_crop_area_camf(x3f, "ActiveImageArea", &expanded, 0, image))
      *image = expanded;
    original_image = expanded;
  }
  else if (denoise && !run_denoising(x3f)) return 0;

  if (encoding != NONE &&
      !convert_data(x3f, &original_image, &il, encoding, apply_sgain, wb)) {
    free(image->buf);
    return 0;
  }

  if (ilevels) *ilevels = il;
  return 1;
}

/* extern */ int x3f_get_preview(x3f_t *x3f,
				 x3f_area16_t *image,
				 x3f_image_levels_t *ilevels,
				 x3f_color_encoding_t encoding,
				 int apply_sgain,
				 char *wb,
				 uint32_t max_width,
				 x3f_area8_t *preview)
{
  int row, col, color;
  uint16_t max_out = 255;

  double conv_matrix[9];
  double lut[LUTSIZE];
  x3f_spatial_gain_corr_t sgain[MAXCORR];
  int sgain_num;
  char *sensorid = NULL;
  int reconstruct_highlights;

  int reduction, reduction2;

  if (image->channels < 3) return 0;

  if (!get_conv(x3f, encoding, wb, LUTSIZE, max_out, lut, conv_matrix))
    return 0;

  reconstruct_highlights = encoding == SRGB &&
    x3f_get_prop_entry(x3f, "SENSORID", &sensorid) && sensorid &&
    !strcmp(sensorid, "F20");

  if (apply_sgain) {
    sgain_num = x3f_get_spatial_gain(x3f, wb, sgain);
    if (sgain_num == 0)
      x3f_printf(WARN, "Could not get spatial gain\n");
  } else {
    sgain_num = 0;
  }

  reduction = (image->columns + max_width - 1)/max_width;
  reduction2 = reduction*reduction;
  preview->columns = image->columns/reduction;
  preview->rows = image->rows/reduction;
  preview->channels = 3;
  preview->row_stride = preview->columns*preview->channels;
  preview->data = preview->buf =
    malloc(preview->rows*preview->row_stride*sizeof(uint8_t));

  for (row = 0; row < preview->rows; row++) {
    for (col = 0; col < preview->columns; col++) {
      double input[3], output[3];

      /* Get the data */
      for (color = 0; color < 3; color++) {
	uint32_t acc = 0;
	int r, c;

	for (r=0; r<reduction; r++)
	  for (c=0; c<reduction; c++)
	    acc += image->data[image->row_stride*(row*reduction + r) +
			       image->channels*(col*reduction + c) + color];

	input[color] = x3f_calc_spatial_gain(sgain, sgain_num,
					     row, col, color,
					     preview->rows, preview->columns) *
	  ((double)acc/reduction2 - ilevels->black[color]) /
	  (ilevels->white[color] - ilevels->black[color]);
      }

      /* Do color conversion */
      x3f_3x3_3x1_mul(conv_matrix, input, output);
      if (reconstruct_highlights) {
	if (getenv("FAST_SIGMA_ENABLE_LEGACY_SHADOW_CORRECTION"))
	  correct_merrill_shadow_green(output);
      }

      /* Write back the data, doing non linear coding */
      for (color = 0; color < 3; color++)
	preview->data[preview->row_stride*row + preview->channels*col + color] =
	  x3f_LUT_lookup(lut, LUTSIZE, output[color]);
    }
  }

  x3f_cleanup_spatial_gain(sgain, sgain_num);

  x3f_crop_area8_camf(x3f, "ActiveImageArea", preview, 1, preview);

  return 1;
}
