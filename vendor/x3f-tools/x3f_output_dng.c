/* X3F_OUTPUT_DNG.C
 *
 * Library for writing the image as DNG.
 *
 * Copyright 2015 - Roland and Erik Karlsson
 * BSD-style - see doc/copyright.txt
 *
 */

#include "x3f_output_dng.h"
#include "x3f_process.h"
#include "x3f_dngtags.h"
#include "x3f_matrix.h"
#include "x3f_meta.h"
#include "x3f_image.h"
#include "x3f_spatial_gain.h"
#include "x3f_printf.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <tiffio.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include <assert.h>

static void vec_double_to_float(double *a, float *b, int len)
{
  int i;

  for (i=0; i<len; i++)
    b[i] = a[i];
}

static void write_identification(x3f_t *x3f, TIFF *tiff, int pipeline)
{
  char *model = NULL;

  TIFFSetField(tiff, TIFFTAG_MAKE, "SIGMA");
  TIFFSetField(tiff, TIFFTAG_SOFTWARE, "fast-sigma-raw 0.1");
  if (x3f_get_prop_entry(x3f, "CAMMODEL", &model) && model && model[0]) {
    TIFFSetField(tiff, TIFFTAG_MODEL, model);
    TIFFSetField(tiff, TIFFTAG_UNIQUECAMERAMODEL, model);
  } else {
    TIFFSetField(tiff, TIFFTAG_MODEL, "SIGMA Foveon");
    TIFFSetField(tiff, TIFFTAG_UNIQUECAMERAMODEL, "SIGMA Foveon");
  }
  if (pipeline == 1)
    TIFFSetField(tiff, TIFFTAG_IMAGEDESCRIPTION,
		 "Merrill developed scene-linear ROMM RGB");
  else if (pipeline == 2)
    TIFFSetField(tiff, TIFFTAG_IMAGEDESCRIPTION,
		 "Normalized gain-corrected Foveon BMT planes");
  else
    TIFFSetField(tiff, TIFFTAG_IMAGEDESCRIPTION,
		 "Foveon preprocessed sensor planes");
}

static uint16_t dng_orientation(const x3f_t *x3f)
{
  switch (x3f->header.rotation) {
  case 90: return ORIENTATION_RIGHTTOP;
  case 180: return ORIENTATION_BOTRIGHT;
  case 270: return ORIENTATION_LEFTBOT;
  default: return ORIENTATION_TOPLEFT;
  }
}

typedef struct {
  char *make;
  char *model;
  char *serial;
  char *white_balance;
  char *flash;
  char *program;
  const char *lens_model;
  char lens_model_fallback[96];
  char datetime[20];
  float lens_specification[4];
  double aperture;
  double exposure_time;
  double exposure_bias;
  double focal_length;
  double subject_distance;
  long focal_length_35mm;
  long iso;
  long metering_mode;
  int have_aperture;
  int have_exposure_time;
  int have_exposure_bias;
  int have_focal_length;
  int have_subject_distance;
  int have_focal_length_35mm;
  int have_iso;
  int have_metering_mode;
  int have_datetime;
  int have_lens_specification;
} capture_metadata_t;

static int get_prop_double(x3f_t *x3f, char *name, double *value)
{
  char *text, *end;
  double parsed;

  if (!x3f_get_prop_entry(x3f, name, &text) || !text) return 0;
  parsed = strtod(text, &end);
  if (end == text || !isfinite(parsed)) return 0;
  *value = parsed;
  return 1;
}

static int get_prop_long(x3f_t *x3f, char *name, int base, long *value)
{
  char *text, *end;
  long parsed;

  if (!x3f_get_prop_entry(x3f, name, &text) || !text) return 0;
  parsed = strtol(text, &end, base);
  if (end == text) return 0;
  *value = parsed;
  return 1;
}

static int get_prop_fraction(x3f_t *x3f, char *name, double *value)
{
  char *text, *end;
  double numerator, denominator;

  if (!x3f_get_prop_entry(x3f, name, &text) || !text) return 0;
  numerator = strtod(text, &end);
  if (end == text || !isfinite(numerator)) return 0;
  if (*end == '/') {
    char *denominator_end;
    denominator = strtod(end + 1, &denominator_end);
    if (denominator_end == end + 1 || denominator == 0.0 ||
        !isfinite(denominator))
      return 0;
    numerator /= denominator;
  }
  *value = numerator;
  return 1;
}

static const char *merrill_lens_model(long id)
{
  switch (id) {
  case 0x1003: return "Sigma 19mm F2.8";
  case 0x1004: return "Sigma 30mm F2.8";
  case 0x1005: return "Sigma 50mm F2.8 Macro";
  case 0x1006: return "Sigma 19mm F2.8";
  case 0x1007: return "Sigma 30mm F2.8";
  case 0x1008: return "Sigma 50mm F2.8 Macro";
  case 0x1009: return "Sigma 14mm F4";
  default: return NULL;
  }
}

static void get_capture_metadata(x3f_t *x3f, capture_metadata_t *meta)
{
  char *focal_text = NULL, *aperture_text = NULL;
  long lens_id, timestamp;
  time_t capture_time;
  struct tm camera_time;

  memset(meta, 0, sizeof(*meta));
  if (!x3f_get_prop_entry(x3f, "CAMMANUF", &meta->make))
    meta->make = "SIGMA";
  if (!x3f_get_prop_entry(x3f, "CAMMODEL", &meta->model))
    meta->model = "SIGMA Foveon";
  x3f_get_prop_entry(x3f, "CAMSERIAL", &meta->serial);
  x3f_get_prop_entry(x3f, "WB_DESC", &meta->white_balance);
  x3f_get_prop_entry(x3f, "FLASH", &meta->flash);
  x3f_get_prop_entry(x3f, "PMODE", &meta->program);

  meta->have_aperture =
    get_prop_double(x3f, "AP_DESC", &meta->aperture) ||
    get_prop_double(x3f, "APERTURE", &meta->aperture);
  meta->have_exposure_time =
    get_prop_fraction(x3f, "SH_DESC", &meta->exposure_time) ||
    get_prop_double(x3f, "SHUTTER", &meta->exposure_time);
  meta->have_exposure_bias =
    get_prop_double(x3f, "EXPCOMP", &meta->exposure_bias);
  meta->have_focal_length =
    get_prop_double(x3f, "FLENGTH", &meta->focal_length);
  meta->have_subject_distance =
    x3f_get_camf_float(x3f, "ObjectDistance", &meta->subject_distance);
  if (meta->have_subject_distance)
    meta->subject_distance /= 100.0; /* CAMF stores centimeters; EXIF uses m. */
  meta->have_focal_length_35mm =
    get_prop_long(x3f, "FLEQ35MM", 10, &meta->focal_length_35mm);
  meta->have_iso = get_prop_long(x3f, "ISO", 10, &meta->iso);
  meta->have_metering_mode = get_prop_long(x3f, "AEMODE", 10,
                                            &meta->metering_mode);

  if (get_prop_long(x3f, "LENSMODEL", 16, &lens_id)) {
    meta->lens_model = merrill_lens_model(lens_id);
    if (meta->lens_model && meta->have_focal_length && meta->have_aperture) {
      meta->lens_specification[0] = (float)meta->focal_length;
      meta->lens_specification[1] = (float)meta->focal_length;
      meta->lens_specification[2] = (float)meta->aperture;
      meta->lens_specification[3] = (float)meta->aperture;
      meta->have_lens_specification = 1;
    }
  }
  if (!meta->lens_model &&
      x3f_get_prop_entry(x3f, "FLENGTH", &focal_text) &&
      x3f_get_prop_entry(x3f, "AP_DESC", &aperture_text)) {
    snprintf(meta->lens_model_fallback, sizeof(meta->lens_model_fallback),
             "SIGMA %smm F%s", focal_text, aperture_text);
    meta->lens_model = meta->lens_model_fallback;
  }

  if (get_prop_long(x3f, "TIME", 10, &timestamp)) {
    /* SIGMA stores the camera's wall-clock fields in a Unix-shaped value. */
    capture_time = (time_t)timestamp;
#if defined(_WIN32) || defined(_WIN64)
    if (gmtime_s(&camera_time, &capture_time) == 0)
#else
    if (gmtime_r(&capture_time, &camera_time) != NULL)
#endif
      meta->have_datetime =
        strftime(meta->datetime, sizeof(meta->datetime),
                 "%Y:%m:%d %H:%M:%S", &camera_time) == 19;
  }
}

static uint16_t exif_exposure_program(const char *program)
{
  if (!program) return 0;
  if (!strcmp(program, "M")) return 1;
  if (!strcmp(program, "P")) return 2;
  if (!strcmp(program, "A")) return 3;
  if (!strcmp(program, "S")) return 4;
  return 0;
}

static uint16_t exif_light_source(const char *wb)
{
  if (!wb) return 0;
  if (!strcmp(wb, "Daylight") || !strcmp(wb, "Sunlight")) return 1;
  if (!strcmp(wb, "Fluorescent") || !strcmp(wb, "Florescent")) return 2;
  if (!strcmp(wb, "Incandescent")) return 3;
  if (!strcmp(wb, "Flash")) return 4;
  if (!strcmp(wb, "Overcast")) return 10;
  if (!strcmp(wb, "Shadow")) return 11;
  return 0;
}

static int write_capture_exif(x3f_t *x3f, TIFF *tiff, uint64_t *offset)
{
  capture_metadata_t meta;
  uint8_t exif_version[4] = {'0', '2', '3', '0'};
  char image_unique_id[33];
  uint16_t value16, iso16;
  int have_unique_id = 0;
  int i;

  get_capture_metadata(x3f, &meta);
  /* libtiff's directory-creation APIs return zero after changing context. */
  TIFFCreateEXIFDirectory(tiff);

  TIFFSetField(tiff, EXIFTAG_EXIFVERSION, exif_version);
  if (meta.have_datetime) {
    TIFFSetField(tiff, EXIFTAG_DATETIMEORIGINAL, meta.datetime);
    TIFFSetField(tiff, EXIFTAG_DATETIMEDIGITIZED, meta.datetime);
  }
  if (meta.have_exposure_time && meta.exposure_time > 0.0) {
    TIFFSetField(tiff, EXIFTAG_EXPOSURETIME, meta.exposure_time);
    TIFFSetField(tiff, EXIFTAG_SHUTTERSPEEDVALUE,
                 -log2(meta.exposure_time));
  }
  if (meta.have_aperture && meta.aperture > 0.0) {
    TIFFSetField(tiff, EXIFTAG_FNUMBER, meta.aperture);
    TIFFSetField(tiff, EXIFTAG_APERTUREVALUE, 2.0 * log2(meta.aperture));
    TIFFSetField(tiff, EXIFTAG_MAXAPERTUREVALUE, 2.0 * log2(meta.aperture));
  }
  value16 = exif_exposure_program(meta.program);
  if (value16) TIFFSetField(tiff, EXIFTAG_EXPOSUREPROGRAM, value16);
  if (meta.have_exposure_bias)
    TIFFSetField(tiff, EXIFTAG_EXPOSUREBIASVALUE, meta.exposure_bias);
  if (meta.have_iso && meta.iso > 0 && meta.iso <= 65535) {
    iso16 = (uint16_t)meta.iso;
    TIFFSetField(tiff, EXIFTAG_ISOSPEEDRATINGS, 1, &iso16);
  }
  if (meta.have_focal_length && meta.focal_length > 0.0)
    TIFFSetField(tiff, EXIFTAG_FOCALLENGTH, meta.focal_length);
  if (meta.have_subject_distance && meta.subject_distance > 0.0)
    TIFFSetField(tiff, EXIFTAG_SUBJECTDISTANCE, meta.subject_distance);
  if (meta.have_focal_length_35mm && meta.focal_length_35mm > 0 &&
      meta.focal_length_35mm <= 65535)
    TIFFSetField(tiff, EXIFTAG_FOCALLENGTHIN35MMFILM,
                 (uint16_t)meta.focal_length_35mm);
  if (meta.have_metering_mode) {
    /* SIGMA mode 8 is its eight-segment/multi-segment metering mode. */
    value16 = meta.metering_mode == 8 ? 5 : 0;
    if (value16) TIFFSetField(tiff, EXIFTAG_METERINGMODE, value16);
  }
  value16 = exif_light_source(meta.white_balance);
  if (value16) TIFFSetField(tiff, EXIFTAG_LIGHTSOURCE, value16);
  TIFFSetField(tiff, EXIFTAG_WHITEBALANCE,
               meta.white_balance && !strcmp(meta.white_balance, "Auto") ? 0 : 1);
  TIFFSetField(tiff, EXIFTAG_FLASH,
               meta.flash && !strcmp(meta.flash, "OFF") ? 16 : 0);
  TIFFSetField(tiff, EXIFTAG_EXPOSUREMODE,
               meta.program && !strcmp(meta.program, "M") ? 1 : 0);
  TIFFSetField(tiff, EXIFTAG_SCENECAPTURETYPE, 0);
  TIFFSetField(tiff, EXIFTAG_COLORSPACE, 1);
  TIFFSetField(tiff, EXIFTAG_PIXELXDIMENSION, x3f->header.columns);
  TIFFSetField(tiff, EXIFTAG_PIXELYDIMENSION, x3f->header.rows);
  if (meta.serial && meta.serial[0])
    TIFFSetField(tiff, EXIFTAG_BODYSERIALNUMBER, meta.serial);
  TIFFSetField(tiff, EXIFTAG_LENSMAKE, "SIGMA");
  if (meta.lens_model)
    TIFFSetField(tiff, EXIFTAG_LENSMODEL, meta.lens_model);
  if (meta.have_lens_specification)
    TIFFSetField(tiff, EXIFTAG_LENSSPECIFICATION,
                 meta.lens_specification);
  for (i = 0; i < 16; ++i) {
    if (x3f->header.unique_identifier[i]) have_unique_id = 1;
    snprintf(image_unique_id + 2 * i, 3, "%02X",
             x3f->header.unique_identifier[i]);
  }
  image_unique_id[32] = '\0';
  if (have_unique_id)
    TIFFSetField(tiff, EXIFTAG_IMAGEUNIQUEID, image_unique_id);

  if (!TIFFWriteCustomDirectory(tiff, offset)) {
    x3f_printf(WARN, "Could not serialize EXIF metadata directory\n");
    return 0;
  }
  return 1;
}

static int get_camf_rect_as_dngrect(x3f_t *x3f, char *name,
				    x3f_area16_t *image, int rescale,
				    uint32_t *rect)
{
  uint32_t camf_rect[4];

  if (!x3f_get_camf_rect(x3f, name, image, rescale, camf_rect))
    return 0;

  /* Translate from Sigma's to Adobe's view on rectangles */
  rect[0] = camf_rect[1];
  rect[1] = camf_rect[0];
  rect[2] = camf_rect[3] + 1;
  rect[3] = camf_rect[2] + 1;

  return 1;
}

static int write_spatial_gain(x3f_t *x3f, x3f_area16_t *image, char *wb,
			      TIFF *tiff)
{
  x3f_spatial_gain_corr_t corr[MAXCORR];
  int corr_num;
  uint32_t active_area[4];
  double originv, originh, scalev, scaleh;

  uint8_t *opcode_list, *p;
  int opcode_size[MAXCORR];
  int opcode_list_size = sizeof(dng_opcodelist_header_t);
  dng_opcodelist_header_t *header;
  int i, j;

  if (!get_camf_rect_as_dngrect(x3f, "ActiveImageArea", image, 1,
				active_area))
    return 0;

  /* Spatial gain in X3F refers to the entire image, but OpcodeList2
     in DNG is appled after cropping to ActiveArea */
  originv = -(double)active_area[0] / (active_area[2] - active_area[0]);
  originh = -(double)active_area[1] / (active_area[3] - active_area[1]);
  scalev =   (double)image->rows    / (active_area[2] - active_area[0]);
  scaleh =   (double)image->columns / (active_area[3] - active_area[1]);

  corr_num = x3f_get_spatial_gain(x3f, wb, corr);
  if (corr_num == 0) return 0;

  for (i=0; i<corr_num; i++) {
    opcode_size[i] = sizeof(dng_opcode_GainMap_t) +
      corr[i].rows*corr[i].cols*corr[i].channels*sizeof(float);
    opcode_list_size += opcode_size[i];
  }

  opcode_list = alloca(opcode_list_size);
  header = (dng_opcodelist_header_t *)opcode_list;
  PUT_BIG_32(header->count, corr_num);

  for (p = opcode_list + sizeof(dng_opcodelist_header_t), i=0;
       i<corr_num;
       p += opcode_size[i], i++) {
    dng_opcode_GainMap_t *gain_map = (dng_opcode_GainMap_t *)p;
    x3f_spatial_gain_corr_t *c = &corr[i];

    PUT_BIG_32(gain_map->header.id, DNG_OPCODE_GAINMAP_ID);
    PUT_BIG_32(gain_map->header.ver, DNG_OPCODE_GAINMAP_VER);
    PUT_BIG_32(gain_map->header.flags, 0);
    PUT_BIG_32(gain_map->header.parsize,
	       opcode_size[i] - sizeof(dng_opcode_header_t));

    PUT_BIG_32(gain_map->Top, c->rowoff);
    PUT_BIG_32(gain_map->Left, c->coloff);
    PUT_BIG_32(gain_map->Bottom, active_area[2] - active_area[0]);
    PUT_BIG_32(gain_map->Right, active_area[3] - active_area[1]);
    PUT_BIG_32(gain_map->Plane, c->chan);
    PUT_BIG_32(gain_map->Planes, c->channels);
    PUT_BIG_32(gain_map->RowPitch, c->rowpitch);
    PUT_BIG_32(gain_map->ColPitch, c->colpitch);
    PUT_BIG_32(gain_map->MapPointsV, c->rows);
    PUT_BIG_32(gain_map->MapPointsH, c->cols);
    PUT_BIG_64(gain_map->MapSpacingV, scalev/(c->rows-1));
    PUT_BIG_64(gain_map->MapSpacingH, scaleh/(c->cols-1));
    PUT_BIG_64(gain_map->MapOriginV, originv);
    PUT_BIG_64(gain_map->MapOriginH, originh);
    PUT_BIG_32(gain_map->MapPlanes, c->channels);

    for (j=0; j<c->rows*c->cols*c->channels; j++)
      PUT_BIG_32(gain_map->MapGain[j], c->gain[j]);
  }

  x3f_cleanup_spatial_gain(corr, corr_num);

  TIFFSetField(tiff, TIFFTAG_OPCODELIST2, opcode_list_size, opcode_list);

  return 1;
}

typedef struct {
  char *name;
  int (*get_bmt_to_xyz)(x3f_t *x3f, char *wb, double *raw_to_xyz);
  double *grayscale_mix;
} camera_profile_t;

/* TODO: more mixes should be defined */
static double grayscale_mix_std[3] = {1.0/3.0, 1.0/3.0, 1.0/3.0};
static double grayscale_mix_red[3] = {2.0, -1.0, 0.0};
static double grayscale_mix_blue[3] = {0.0, -1.0, 2.0};

static int get_bmt_to_xyz_noconvert(x3f_t *x3f, char *wb, double *bmt_to_xyz)
{
  /* TODO: assuming working space to be Adobe RGB. Is that acceptable? */
  x3f_AdobeRGB_to_XYZ(bmt_to_xyz);
  return 1;
}

static const camera_profile_t camera_profiles[] = {
  {"Default", x3f_get_bmt_to_xyz, NULL},
  {"Grayscale", get_bmt_to_xyz_noconvert, grayscale_mix_std},
  {"Grayscale (red filter)", get_bmt_to_xyz_noconvert, grayscale_mix_red},
  {"Grayscale (blue filter)", get_bmt_to_xyz_noconvert, grayscale_mix_blue},
  {"Unconverted", get_bmt_to_xyz_noconvert, NULL},
};

/* Photo Pro matrices Merrill data into its D50 ROMM/RIMM working space before
   ColorDQ and tone processing.  Return the D65-referenced equivalent expected
   by write_camera_profile(); its ForwardMatrix is adapted back to D50. */
static int get_linear_romm_to_xyz(x3f_t *x3f, char *wb, double *romm_to_xyz)
{
  double romm_to_d50[9], d50_to_d65[9];
  (void)x3f;
  (void)wb;
  x3f_ProPhotoRGB_to_XYZ(romm_to_d50);
  x3f_Bradford_D50_to_D65(d50_to_d65);
  x3f_3x3_3x3_mul(d50_to_d65, romm_to_d50, romm_to_xyz);
  return 1;
}

static const camera_profile_t linear_romm_profile[] = {
  {"Linear ROMM (Merrill Neutral)", get_linear_romm_to_xyz, NULL},
};

/*
 * Keep the default Lightroom rendering neutral and editable.  The recovered
 * Photo Pro Standard curve has a steep toe and shoulder which, combined with
 * BaselineExposure, creates excessive default contrast in Adobe hosts.  These
 * collinear points explicitly request an identity curve under cubic-spline
 * interpolation instead of allowing the host to choose an implicit curve.
 */
static const float merrill_neutral_tone_curve[] = {
  0.00f, 0.00f,
  0.25f, 0.25f,
  0.50f, 0.50f,
  0.75f, 0.75f,
  1.00f, 1.00f,
};

static int write_camera_profile(x3f_t *x3f, char *wb,
				const camera_profile_t *profile,
				TIFF *tiff)
{
  double bmt_to_xyz[9], xyz_to_bmt[9], bmt_to_d50[9];
  float color_matrix1[9], forward_matrix1[9];

  if (!profile->get_bmt_to_xyz(x3f, wb, bmt_to_xyz)) {
    x3f_printf(ERR, "Could not get bmt_to_xyz for white balance: %s\n", wb);
    return 0;
  }
  x3f_3x3_inverse(bmt_to_xyz, xyz_to_bmt);
  vec_double_to_float(xyz_to_bmt, color_matrix1, 9);
  TIFFSetField(tiff, TIFFTAG_COLORMATRIX1, 9, color_matrix1);
  /* All matrices returned here use a D65 XYZ reference white. */
  TIFFSetField(tiff, TIFFTAG_CALIBRATIONILLUMINANT1, 21);

  if (profile->grayscale_mix) {
    double d50_xyz[3] = {0.96422, 1.00000, 0.82521};
    double grayscale_mix_mat[9], ones[9], d50_xyz_mat[9], bmt_to_grayscale[9];

    x3f_3x3_diag(profile->grayscale_mix, grayscale_mix_mat);
    x3f_3x3_ones(ones);
    x3f_3x3_3x3_mul(ones, grayscale_mix_mat, bmt_to_grayscale);
    x3f_3x3_diag(d50_xyz, d50_xyz_mat);
    x3f_3x3_3x3_mul(d50_xyz_mat, bmt_to_grayscale, bmt_to_d50);
  }
  else {
    double d65_to_d50[9];

    x3f_Bradford_D65_to_D50(d65_to_d50);
    x3f_3x3_3x3_mul(d65_to_d50, bmt_to_xyz, bmt_to_d50);
  }
  vec_double_to_float(bmt_to_d50, forward_matrix1, 9);
  TIFFSetField(tiff, TIFFTAG_FORWARDMATRIX1, 9, forward_matrix1);

  TIFFSetField(tiff, TIFFTAG_PROFILENAME, profile->name);
  /* Tell the raw converter to refrain from clipping the dark areas */
  TIFFSetField(tiff, TIFFTAG_DEFAULTBLACKRENDER, 1);

  return 1;
}

#if defined(_WIN32) || defined (_WIN64)
/* tmpfile() is broken on Windows */
#include <windows.h>

#define tmpfile tmpfile_win
static FILE *tmpfile_win(void)
{
  char dir[MAX_PATH], file[MAX_PATH];
  HANDLE h;

  if (!GetTempPath(MAX_PATH, dir)) return NULL;
  if (!GetTempFileName(dir, "x3f", 0, file)) return NULL;

  h = CreateFile(file, GENERIC_READ | GENERIC_WRITE, 0, NULL, CREATE_ALWAYS,
		 FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, NULL);
  if (h == INVALID_HANDLE_VALUE) return NULL;

  return fdopen(_open_osfhandle((intptr_t)h, O_RDWR | O_BINARY), "w+b");
}
#endif

static x3f_return_t write_camera_profiles(x3f_t *x3f, char *wb,
					  const camera_profile_t *profiles,
					  int num,
					  TIFF *tiff)
{
  FILE *tiff_file;
  uint32_t *profile_offsets;
  int i;

  assert(num >= 1);
  if (!write_camera_profile(x3f, wb, &profiles[0], tiff))
    return X3F_ARGUMENT_ERROR;
  TIFFSetField(tiff, TIFFTAG_ASSHOTPROFILENAME, profiles[0].name);
  if (num == 1) return X3F_OK;

  profile_offsets = alloca((num-1)*sizeof(uint32_t));

  tiff_file = fdopen(dup(TIFFFileno(tiff)), "w+b");
  if (!tiff_file) return X3F_OUTFILE_ERROR;

  for (i=1; i < num; i++) {
    FILE *tmp;
    TIFF *tmp_tiff;
#define BUFSIZE 1024
    char buf[BUFSIZE];
    int offset, count;

    if (!(tmp = tmpfile())) {
      fclose(tiff_file);
      return X3F_OUTFILE_ERROR;
    }
    if (!(tmp_tiff = TIFFFdOpen(dup(fileno(tmp)), "", "wb"))) { /* Big endian */
      fclose(tmp);
      fclose(tiff_file);
      return X3F_OUTFILE_ERROR;
    }
    if (!write_camera_profile(x3f, wb, &profiles[i], tmp_tiff)) {
      fclose(tmp);
      TIFFClose(tmp_tiff);
      fclose(tiff_file);
      return X3F_ARGUMENT_ERROR;
    }
    TIFFWriteDirectory(tmp_tiff);
    TIFFClose(tmp_tiff);

    fseek(tiff_file, 0, SEEK_END);
    offset = (ftell(tiff_file)+1) & ~1; /* 2-byte alignment */
    fseek(tiff_file, offset, SEEK_SET);
    profile_offsets[i-1] = offset;

    fputs("MMCR", tiff_file);	/* DNG camera profile magic in big endian */
    fseek(tmp, 4, SEEK_SET);	/* Skip over the standard TIFF magic */

    while((count = fread(buf, 1, BUFSIZE, tmp)))
      fwrite(buf, 1, count, tiff_file);

    fclose(tmp);
  }

  fclose(tiff_file);
  TIFFSetField(tiff, TIFFTAG_EXTRACAMERAPROFILES, num-1, profile_offsets);
  return X3F_OK;
}

#if defined(_WIN32) || defined(_WIN64)
#define BINMODE O_BINARY
#else
#define BINMODE 0
#endif

static x3f_return_t dump_data_as_dng(x3f_t *x3f,
				     char *outfilename,
				     int fix_bad,
				     int denoise,
				     int apply_sgain,
				     char *wb,
				     int compress,
				     int pipeline)
{
  x3f_return_t ret;
  int fd = open(outfilename, O_RDWR | BINMODE | O_CREAT | O_TRUNC, 0644);
  TIFF *f_out;
  uint64_t sub_ifds[1] = {0};
  uint64_t exif_ifd = 0;

  double sensor_iso, capture_iso;
  double gain[3], gain_inv[3], gain_inv_mat[9];
  float as_shot_neutral[3], camera_calibration1[9];
  float black_level[3];
  uint32_t active_area[4];
  x3f_area16_t image;
  x3f_area16_t developed_crop;
  x3f_image_levels_t ilevels;
  x3f_area8_t preview;
  capture_metadata_t capture_metadata;
  int row;

  x3f_install_dng_tags();

  if (fd == -1) return X3F_OUTFILE_ERROR;
  if (!(f_out = TIFFFdOpen(fd, outfilename, "w"))) {
    close(fd);
    return X3F_OUTFILE_ERROR;
  }

  if (wb == NULL) wb = x3f_get_wb(x3f);
  get_capture_metadata(x3f, &capture_metadata);
  if (!x3f_get_image(x3f, &image, &ilevels, NONE, 0,
		     fix_bad, denoise, apply_sgain, wb) ||
      image.channels != 3) {
    x3f_printf(ERR, "Could not get image\n");
    TIFFClose(f_out);
    return X3F_ARGUMENT_ERROR;
  }
  if (!x3f_get_preview(x3f, &image, &ilevels, SRGB,
		       apply_sgain, wb, 300, &preview)) {
    x3f_printf(ERR, "Could not get preview\n");
    TIFFClose(f_out);
    free(image.buf);
    return X3F_ARGUMENT_ERROR;
  }

  if (pipeline != 0) {
    x3f_color_encoding_t encoding = pipeline == 1 ? LINEAR_ROMM : LINEAR_BMT;
    if (!x3f_convert_image(x3f, &image, &ilevels, encoding,
			   apply_sgain, wb)) {
      x3f_printf(ERR, "Could not create normalized linear image\n");
      TIFFClose(f_out);
      free(image.buf);
      free(preview.buf);
      return X3F_ARGUMENT_ERROR;
    }
    if (x3f_crop_area_camf(x3f, "ActiveImageArea", &image, 1,
			   &developed_crop))
      image = developed_crop;
  }

  TIFFSetField(f_out, TIFFTAG_SUBFILETYPE, FILETYPE_REDUCEDIMAGE);
  write_identification(x3f, f_out, pipeline);
  if (capture_metadata.have_datetime)
    TIFFSetField(f_out, TIFFTAG_DATETIME, capture_metadata.datetime);
  if (capture_metadata.serial && capture_metadata.serial[0])
    TIFFSetField(f_out, TIFFTAG_CAMERASERIALNUMBER,
                 capture_metadata.serial);
  if (capture_metadata.have_lens_specification)
    TIFFSetField(f_out, TIFFTAG_LENSINFO,
                 capture_metadata.lens_specification);
  TIFFSetField(f_out, TIFFTAG_IMAGEWIDTH, preview.columns);
  TIFFSetField(f_out, TIFFTAG_IMAGELENGTH, preview.rows);
  TIFFSetField(f_out, TIFFTAG_ROWSPERSTRIP, preview.rows);
  TIFFSetField(f_out, TIFFTAG_SAMPLESPERPIXEL, preview.channels);
  TIFFSetField(f_out, TIFFTAG_BITSPERSAMPLE, 8);
  TIFFSetField(f_out, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(f_out, TIFFTAG_COMPRESSION, COMPRESSION_NONE);
  TIFFSetField(f_out, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_RGB);
  TIFFSetField(f_out, TIFFTAG_ORIENTATION, dng_orientation(x3f));
  TIFFSetField(f_out, TIFFTAG_DNGVERSION, "\001\004\000\000");
  TIFFSetField(f_out, TIFFTAG_DNGBACKWARDVERSION,
	       compress ? "\001\004\000\000" : "\001\003\000\000");
  TIFFSetField(f_out, TIFFTAG_SUBIFD, 1, sub_ifds);
  /* Reserve the pointer so attaching the EXIF IFD can rewrite IFD0 safely. */
  TIFFSetField(f_out, TIFFTAG_EXIFIFD, exif_ifd);

  if (pipeline == 1) {
    /*
     * Merrill's normalized scene-linear output deliberately retains roughly
     * 1.5 EV of highlight headroom.  Without a baseline offset Lightroom
     * treats that headroom as the default rendering and opens the DNG much
     * darker than Photo Pro.  This value is the robust median measured over
     * matching neutralized DNG/SPP TIFF pairs; it changes only the host's
     * initial exposure zero point, not the stored linear samples.
     */
    TIFFSetField(f_out, TIFFTAG_BASELINEEXPOSURE, 1.5);
  }
  else if (pipeline == 0 &&
           x3f_get_camf_float(x3f, "SensorISO", &sensor_iso) &&
           x3f_get_camf_float(x3f, "CaptureISO", &capture_iso)) {
    double baseline_exposure = log2(capture_iso/sensor_iso);
    TIFFSetField(f_out, TIFFTAG_BASELINEEXPOSURE, baseline_exposure);
  }

  ret = pipeline == 1
    ? write_camera_profiles(x3f, wb, linear_romm_profile, 1, f_out)
    : write_camera_profiles(x3f, wb, camera_profiles,
			    sizeof(camera_profiles)/sizeof(camera_profile_t), f_out);
  if (ret != X3F_OK) {
    x3f_printf(ERR, "Could not write camera profiles\n");
    TIFFClose(f_out);
    free(image.buf);
    free(preview.buf);
    return ret;
  }
  if (pipeline == 1)
    TIFFSetField(f_out, TIFFTAG_PROFILETONECURVE,
		 sizeof(merrill_neutral_tone_curve) / sizeof(float),
		 merrill_neutral_tone_curve);

  if (pipeline != 0) {
    as_shot_neutral[0] = as_shot_neutral[1] = as_shot_neutral[2] = 1.0f;
    x3f_3x3_identity(gain_inv_mat);
    vec_double_to_float(gain_inv_mat, camera_calibration1, 9);
  } else if (!x3f_get_gain(x3f, wb, gain)) {
    x3f_printf(ERR, "Could not get gain for white balance: %s\n", wb);
    TIFFClose(f_out);
    free(image.buf);
    free(preview.buf);
    return X3F_ARGUMENT_ERROR;
  } else {
    x3f_3x1_invert(gain, gain_inv);
    vec_double_to_float(gain_inv, as_shot_neutral, 3);
  }
  TIFFSetField(f_out, TIFFTAG_ASSHOTNEUTRAL, 3, as_shot_neutral);

#define WB_D65 "Overcast"
  if (pipeline == 0 && !x3f_get_gain(x3f, WB_D65, gain)) {
    x3f_printf(ERR, "Could not get gain for white balance: %s\n", WB_D65);
    TIFFClose(f_out);
    free(image.buf);
    free(preview.buf);
    return X3F_ARGUMENT_ERROR;
  }
  if (pipeline == 0) {
    x3f_3x1_invert(gain, gain_inv);
    x3f_3x3_diag(gain_inv, gain_inv_mat);
    vec_double_to_float(gain_inv_mat, camera_calibration1, 9);
  }
  TIFFSetField(f_out, TIFFTAG_CAMERACALIBRATION1, 9, camera_calibration1);

  for (row=0; row < preview.rows; row++)
    TIFFWriteScanline(f_out, preview.data + preview.row_stride*row, row, 0);

  TIFFWriteDirectory(f_out);

  TIFFSetField(f_out, TIFFTAG_SUBFILETYPE, 0);
  TIFFSetField(f_out, TIFFTAG_IMAGEWIDTH, image.columns);
  TIFFSetField(f_out, TIFFTAG_IMAGELENGTH, image.rows);
  TIFFSetField(f_out, TIFFTAG_ROWSPERSTRIP, 32);
  TIFFSetField(f_out, TIFFTAG_SAMPLESPERPIXEL, 3);
  TIFFSetField(f_out, TIFFTAG_BITSPERSAMPLE, 16);
  TIFFSetField(f_out, TIFFTAG_PLANARCONFIG, PLANARCONFIG_CONTIG);
  TIFFSetField(f_out, TIFFTAG_COMPRESSION,
	       compress ? COMPRESSION_ADOBE_DEFLATE : COMPRESSION_NONE);
  if (compress)
    TIFFSetField(f_out, TIFFTAG_PREDICTOR, PREDICTOR_HORIZONTAL);
  TIFFSetField(f_out, TIFFTAG_PHOTOMETRIC, PHOTOMETRIC_LINEARRAW);
  /* Prevent further chroma denoising in DNG processing software */
  TIFFSetField(f_out, TIFFTAG_CHROMABLURRADIUS, 0.0);

  vec_double_to_float(ilevels.black, black_level, 3);
  TIFFSetField(f_out, TIFFTAG_BLACKLEVEL, 3, black_level);
  TIFFSetField(f_out, TIFFTAG_WHITELEVEL, 3, ilevels.white);

  if (pipeline == 0 && apply_sgain)
    if (!write_spatial_gain(x3f, &image, wb, f_out))
      x3f_printf(WARN, "Could not get spatial gain\n");

  if (pipeline == 0 &&
      get_camf_rect_as_dngrect(x3f, "ActiveImageArea", &image, 1, active_area))
    TIFFSetField(f_out, TIFFTAG_ACTIVEAREA, active_area);

  for (row=0; row < image.rows; row++)
    TIFFWriteScanline(f_out, image.data + image.row_stride*row, row, 0);

  TIFFWriteDirectory(f_out);

  if (write_capture_exif(x3f, f_out, &exif_ifd)) {
    if (!TIFFSetDirectory(f_out, 0) ||
        !TIFFSetField(f_out, TIFFTAG_EXIFIFD, exif_ifd) ||
        !TIFFRewriteDirectory(f_out))
      x3f_printf(WARN, "Could not attach EXIF metadata directory\n");
  } else {
    x3f_printf(WARN, "Could not write EXIF metadata directory\n");
  }
  TIFFClose(f_out);
  free(image.buf);
  free(preview.buf);

  return X3F_OK;
}

/* extern */
x3f_return_t x3f_dump_raw_data_as_dng(x3f_t *x3f, char *outfilename,
				      int fix_bad, int denoise,
				      int apply_sgain, char *wb,
				      int compress)
{
  return dump_data_as_dng(x3f, outfilename, fix_bad, denoise, apply_sgain,
			  wb, compress, 0);
}

/* extern */
x3f_return_t x3f_dump_developed_data_as_dng(x3f_t *x3f, char *outfilename,
					    int fix_bad, int denoise,
					    int apply_sgain, char *wb,
					    int compress)
{
  return dump_data_as_dng(x3f, outfilename, fix_bad, denoise, apply_sgain,
			  wb, compress, 1);
}

/* extern */
x3f_return_t x3f_dump_bmt_data_as_dng(x3f_t *x3f, char *outfilename,
				      int fix_bad, int denoise,
				      int apply_sgain, char *wb,
				      int compress)
{
  return dump_data_as_dng(x3f, outfilename, fix_bad, denoise, apply_sgain,
			  wb, compress, 2);
}
