/*
 * Thin, dependency-free bridge used by the Rust CLI.  The X3F decoder and
 * calibration code beside this file retains its original BSD-style license.
 */
#include "x3f_denoise.h"
#include "x3f_io.h"
#include "x3f_matrix.h"
#include "x3f_meta.h"
#include "x3f_output_dng.h"
#include "x3f_printf.h"
#include "x3f_process.h"

#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#else
#include <unistd.h>
#endif

static unsigned int fsr_processor_count(void) {
#if defined(_WIN32) || defined(_WIN64)
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  return info.dwNumberOfProcessors > 0
             ? (unsigned int)info.dwNumberOfProcessors
             : 1;
#else
  long online = sysconf(_SC_NPROCESSORS_ONLN);
  return online > 0 ? (unsigned int)online : 1;
#endif
}

enum {
  FSR_OPEN_INPUT = 100,
  FSR_PARSE_INPUT,
  FSR_NO_CAMF,
  FSR_LOAD_CAMF,
  FSR_LOAD_PROP,
  FSR_NO_RAW,
  FSR_LOAD_RAW
};

int fsr_convert(const char *input, const char *output, int compress,
                int fix_bad_pixels, int spatial_gain_mode, int pipeline) {
  FILE *file = fopen(input, "rb");
  x3f_t *x3f;
  x3f_directory_entry_t *entry;
  x3f_return_t result;
  int spatial_gain;

  if (getenv("FAST_SIGMA_DEBUG")) x3f_printf_level = DEBUG;

  if (!file) return FSR_OPEN_INPUT;
  x3f = x3f_new_from_file(file);
  if (!x3f) {
    fclose(file);
    return FSR_PARSE_INPUT;
  }

  entry = x3f_get_camf(x3f);
  if (!entry) {
    result = FSR_NO_CAMF;
    goto done;
  }
  if (x3f_load_data(x3f, entry) != X3F_OK) {
    result = FSR_LOAD_CAMF;
    goto done;
  }
  if (getenv("FAST_SIGMA_DEBUG")) {
    double color_dq[3];
    double cc[9];
    static const char *noise_names[] = {
      "ISONoiseSettings", "AdditionalISONoiseSettings", "ColorNoiseModel"
    };
    unsigned int noise_index;
    if (x3f_get_camf_float_vector(x3f, "ColorDQCamRGB", color_dq))
      x3f_printf(DEBUG, "ColorDQCamRGB = [%g %g %g]\n",
                 color_dq[0], color_dq[1], color_dq[2]);
    if (x3f_get_camf_matrix_for_wb(x3f, "WhiteBalanceColorCorrections",
                                   x3f_get_wb(x3f), 3, 3, cc)) {
      x3f_printf(DEBUG, "WhiteBalanceColorCorrection\n");
      x3f_3x3_print(DEBUG, cc);
    }
    for (noise_index = 0; noise_index < 3; ++noise_index) {
      int rows, columns;
      double *settings;
      if (x3f_get_camf_matrix_var(x3f, (char *)noise_names[noise_index],
                                  &rows, &columns, NULL, M_FLOAT,
                                  (void **)&settings)) {
        int row, column;
        x3f_printf(DEBUG, "%s dimensions = %d x %d\n",
                   noise_names[noise_index], rows, columns);
        for (row = 0; row < rows; ++row) {
          x3f_printf(DEBUG, "%s[%d]", noise_names[noise_index], row);
          for (column = 0; column < columns; ++column)
            x3f_printf(DEBUG, " %g", settings[row * columns + column]);
          x3f_printf(DEBUG, "\n");
        }
      }
    }
  }

  entry = x3f_get_prop(x3f);
  if (entry && x3f_load_data(x3f, entry) != X3F_OK) {
    result = FSR_LOAD_PROP;
    goto done;
  }

  entry = x3f_get_raw(x3f);
  if (!entry) {
    result = FSR_NO_RAW;
    goto done;
  }
  if (x3f_load_data(x3f, entry) != X3F_OK) {
    result = FSR_LOAD_RAW;
    goto done;
  }

  spatial_gain = spatial_gain_mode < 0
    ? x3f->header.version < X3F_VERSION_4_0
    : spatial_gain_mode != 0;
  if (pipeline == 1)
    result = x3f_dump_developed_data_as_dng(
      x3f, (char *)output, fix_bad_pixels != 0, 1, spatial_gain,
      NULL, compress != 0);
  else if (pipeline == 2)
    result = x3f_dump_bmt_data_as_dng(
      x3f, (char *)output, fix_bad_pixels != 0, 0, spatial_gain,
      NULL, compress != 0);
  else
    result = x3f_dump_raw_data_as_dng(
      x3f, (char *)output, fix_bad_pixels != 0, 0, spatial_gain,
      NULL, compress != 0);

done:
  x3f_delete(x3f);
  fclose(file);
  return result;
}

const char *fsr_error_string(int code) {
  switch (code) {
  case X3F_OK: return "success";
  case X3F_ARGUMENT_ERROR: return "unsupported or incomplete X3F metadata";
  case X3F_INFILE_ERROR: return "error reading X3F data";
  case X3F_OUTFILE_ERROR: return "error writing DNG output";
  case X3F_INTERNAL_ERROR: return "internal decoder error";
  case FSR_OPEN_INPUT: return "cannot open input file";
  case FSR_PARSE_INPUT: return "not a valid or supported X3F file";
  case FSR_NO_CAMF: return "X3F has no camera calibration section";
  case FSR_LOAD_CAMF: return "cannot decode camera calibration section";
  case FSR_LOAD_PROP: return "cannot decode X3F properties";
  case FSR_NO_RAW: return "X3F has no supported Foveon raw image";
  case FSR_LOAD_RAW: return "cannot decode Foveon raw image";
  default: return "unknown conversion error";
  }
}

/*
 * Merrill's developed path cannot matrix the three Foveon planes directly:
 * the large signed color-correction matrix turns fine inter-plane noise into
 * clipped RGB chroma.  Photo Pro uses a considerably larger edge-aware
 * residual pipeline in FPixelProc2_F20::DenoiseF20.  This compact domain-
 * transform approximation follows the same important invariants: only B-T
 * and B-2M+T are smoothed, the T plane remains bit-exact, and range decisions
 * use Merrill's CAMF ColorNoiseModel and ISO-interpolated NoiseScaling.  The
 * spatial recursion is still a compact substitute for Photo Pro's 21-pixel
 * local-mean/residual system. Raw and BMT DNG pipelines do not call it.
 */
static int16_t filter_step(int16_t value, int16_t neighbor, float weight) {
  float result = value + weight * ((float)neighbor - value);
  long filtered = (long)(result < 0.0f ? result - 0.5f : result + 0.5f);
  return (int16_t)(filtered < -32768 ? -32768
                   : filtered > 32767 ? 32767 : filtered);
}

static unsigned int residual_edge(uint16_t y0, uint16_t y1,
                                  int16_t u0, int16_t u1,
                                  int16_t v0, int16_t v1,
                                  float chroma_edge_weight) {
  unsigned int luminance = y0 > y1 ? y0 - y1 : y1 - y0;
  if (chroma_edge_weight <= 0.0f) return luminance;
  unsigned int du = (unsigned int)abs((int)u0 - (int)u1);
  unsigned int dv = (unsigned int)abs((int)v0 - (int)v1);
  unsigned int chroma = (unsigned int)(chroma_edge_weight *
    (float)(du > dv ? du : dv));
  return luminance > chroma ? luminance : chroma;
}

static unsigned int merrill_range_index(const uint16_t *left,
                                        const uint16_t *right,
                                        const double *noise_model,
                                        const double *sample_scale,
                                        double bilateral_scale) {
  double distance = 0.000000015;
  unsigned int channel;
  for (channel = 0; channel < 3; ++channel) {
    double center = left[channel] / sample_scale[channel];
    double neighbor = right[channel] / sample_scale[channel];
    double delta = neighbor - center;
    double variance = noise_model[channel]
      + noise_model[3 + channel] * fabs(center)
      + noise_model[6 + channel] * center * center;
    if (variance > 0.0)
      distance += bilateral_scale * delta * delta / variance;
  }
  if (distance >= 2048.0) return 2048;
  return (unsigned int)distance;
}

static int merrill_interpolate_noise_row(x3f_t *x3f, const char *name,
                                         double *result) {
  double *settings, capture_iso;
  int rows, columns, row, column;
  if (!x3f_get_camf_matrix_var(x3f, (char *)name, &rows, &columns,
                               NULL, M_FLOAT, (void **)&settings) ||
      columns != 13 || rows < 1 ||
      !x3f_get_camf_float(x3f, "CaptureISO", &capture_iso))
    return 0;
  if (capture_iso <= settings[0]) {
    for (column = 0; column < 13; ++column)
      result[column] = settings[column];
    return 1;
  }
  for (row = 1; row < rows; ++row) {
    double high_iso = settings[13 * row];
    if (capture_iso <= high_iso) {
      double low_iso = settings[13 * (row - 1)];
      double fraction = (capture_iso - low_iso) / (high_iso - low_iso);
      for (column = 0; column < 13; ++column)
        result[column] = settings[13 * (row - 1) + column] *
          (1.0 - fraction) + settings[13 * row + column] * fraction;
      return 1;
    }
  }
  for (column = 0; column < 13; ++column)
    result[column] = settings[13 * (rows - 1) + column];
  return 1;
}

static double merrill_noise_scaling(x3f_t *x3f) {
  double settings[13];
  return merrill_interpolate_noise_row(x3f, "ISONoiseSettings", settings)
    ? settings[4] : 0.005;
}

typedef struct {
  const x3f_area16_t *image;
  uint16_t *output;
  const float *weight;
  float model[9];
  float sample_scale[3];
  float bilateral_scale;
  unsigned int radius, sample_step;
  uint32_t row_begin, row_end;
} merrill_bilateral_job_t;

static void *merrill_bilateral_worker(void *opaque) {
  merrill_bilateral_job_t *job = opaque;
  const x3f_area16_t *image = job->image;
  uint32_t row, col;
  for (row = job->row_begin; row < job->row_end; ++row) {
    for (col = job->radius; col + job->radius < image->columns; ++col) {
      const uint16_t *center = image->data + row * image->row_stride +
                               col * image->channels;
      float c[3], inverse_variance[3], sum[3] = {0.0f, 0.0f, 0.0f};
      float weight_sum = 0.0f;
      int dy, dx;
      unsigned int channel;
      for (channel = 0; channel < 3; ++channel) {
        c[channel] = center[channel] / job->sample_scale[channel];
        inverse_variance[channel] = job->bilateral_scale /
          (job->model[channel] + job->model[3 + channel] * fabsf(c[channel]) +
           job->model[6 + channel] * c[channel] * c[channel]);
      }
      for (dy = -(int)job->radius; dy <= (int)job->radius;
           dy += (int)job->sample_step) {
        const uint16_t *neighbor = image->data +
          (uint32_t)((int)row + dy) * image->row_stride +
          (col - job->radius) * image->channels;
        for (dx = -(int)job->radius; dx <= (int)job->radius;
             dx += (int)job->sample_step,
             neighbor += image->channels * job->sample_step) {
          float distance = 0.000000015f;
          unsigned int index;
          for (channel = 0; channel < 3; ++channel) {
            float value = neighbor[channel] / job->sample_scale[channel];
            float delta = value - c[channel];
            distance += delta * delta * inverse_variance[channel];
          }
          if (distance >= 2048.0f) continue;
          index = (unsigned int)distance;
          for (channel = 0; channel < 3; ++channel)
            sum[channel] += neighbor[channel] * job->weight[index];
          weight_sum += job->weight[index];
        }
      }
      if (weight_sum > 0.0f) {
        uint16_t *out = job->output +
          ((size_t)row * image->columns + col) * 3;
        for (channel = 0; channel < 3; ++channel) {
          long value = lrintf(sum[channel] / weight_sum);
          out[channel] = (uint16_t)(value < 0 ? 0 :
                                    value > 16383 ? 16383 : value);
        }
      }
    }
  }
  return NULL;
}

/* Exact finite-window F20 bilateral pass.  At the Merrill ISO-100 setting
   this is a 21x21 window sampled on a two-pixel lattice. */
static int merrill_bilateral_rgb_exact(x3f_area16_t *image,
                                       const double *noise_model,
                                       const double *sample_scale,
                                       double bilateral_scale,
                                       unsigned int radius,
                                       unsigned int subsample) {
  uint16_t *output = NULL;
  float weight[2048];
  merrill_bilateral_job_t *jobs = NULL;
  pthread_t *threads = NULL;
  unsigned int thread_count, t;
  size_t pixels = (size_t)image->rows * image->columns;
  if (!noise_model || radius == 0 || image->rows <= 2 * radius ||
      image->columns <= 2 * radius)
    return 0;
  thread_count = fsr_processor_count();
  if (thread_count > 12) thread_count = 12;
  if (thread_count > image->rows - 2 * radius)
    thread_count = image->rows - 2 * radius;
  output = malloc(3 * pixels * sizeof(*output));
  jobs = calloc(thread_count, sizeof(*jobs));
  threads = calloc(thread_count, sizeof(*threads));
  if (!output || !jobs || !threads) goto failed;
  for (t = 0; t < image->rows; ++t) {
    const uint16_t *source = image->data + t * image->row_stride;
    memcpy(output + (size_t)t * image->columns * 3, source,
           (size_t)image->columns * 3 * sizeof(*output));
  }
  for (t = 0; t < 2048; ++t)
    weight[t] = expf(-0.00390815828f * t);
  for (t = 0; t < thread_count; ++t) {
    unsigned int interior = image->rows - 2 * radius;
    unsigned int channel;
    jobs[t].image = image;
    jobs[t].output = output;
    jobs[t].weight = weight;
    for (channel = 0; channel < 9; ++channel)
      jobs[t].model[channel] = (float)noise_model[channel];
    for (channel = 0; channel < 3; ++channel)
      jobs[t].sample_scale[channel] = (float)sample_scale[channel];
    jobs[t].bilateral_scale = (float)bilateral_scale;
    jobs[t].radius = radius;
    jobs[t].sample_step = subsample ? 2 : 1;
    jobs[t].row_begin = radius + interior * t / thread_count;
    jobs[t].row_end = radius + interior * (t + 1) / thread_count;
    if (pthread_create(&threads[t], NULL, merrill_bilateral_worker, &jobs[t])) {
      jobs[t].row_end = jobs[t].row_begin;
      break;
    }
  }
  while (t) pthread_join(threads[--t], NULL);
  for (t = 0; t < image->rows; ++t)
    memcpy(image->data + t * image->row_stride,
           output + (size_t)t * image->columns * 3,
           (size_t)image->columns * 3 * sizeof(*output));
  free(threads);
  free(jobs);
  free(output);
  return 1;
failed:
  free(threads);
  free(jobs);
  free(output);
  return 0;
}

static float merrill_median5(float a, float b, float c, float d, float e) {
  float values[5] = {a, b, c, d, e};
  unsigned int i, j;
  for (i = 1; i < 5; ++i) {
    float value = values[i];
    for (j = i; j && values[j - 1] > value; --j)
      values[j] = values[j - 1];
    values[j] = value;
  }
  return values[2];
}

/* F20 ComputeEdgeMap (0x63d90) does not build its guide directly from
   adjacent RGB distances.  It first takes a five-sample cross median in each
   sensor plane, measures centered horizontal and vertical gradients two
   pixels apart, optionally averages that response with its four cardinal
   neighbors, and finally indexes EdgeLUT.  The median and directional flags
   are printed by DenoiseF20 but are not consumed by this Merrill call path:
   the cross median is unconditional and the directional argument is always
   zero.  SmoothEdgeMap and Accumulate retain their CAMF-controlled behavior. */
static int merrill_edge_map_exact(x3f_t *x3f, const x3f_area16_t *image,
                                  const double *noise_model,
                                  const double *sample_scale,
                                  float edge_strength, float decay,
                                  float *coefficient) {
  size_t pixels = (size_t)image->rows * image->columns;
  float *median = NULL, *raw_edge = NULL;
  double lin_shift = 10.0, locality = 1.0;
  double capture_iso, sensor_iso;
  float iso_gain = 1.0f;
  int32_t median_flag = 1, directional = 0, smooth = 1;
  int32_t accumulate = 0, use_noise_model = 0;
  float edge_limit, edge_index_scale;
  uint32_t row, col;
  unsigned int channel;

  if (!coefficient || !noise_model || !sample_scale ||
      image->rows < 8 || image->columns < 3)
    return 0;
  x3f_get_camf_float(x3f, "EdgeMapLinShift", &lin_shift);
  x3f_get_camf_float(x3f, "EdgeMapScale", &locality);
  x3f_get_camf_signed(x3f, "EdgeMapDoMedianFilter", &median_flag);
  x3f_get_camf_signed(x3f, "EdgeMapDoDirectionalFilter", &directional);
  x3f_get_camf_signed(x3f, "EdgeMapDoSmoothEdge", &smooth);
  x3f_get_camf_signed(x3f, "EdgeMapAccumulate", &accumulate);
  x3f_get_camf_signed(x3f, "EdgeMapNoiseModel", &use_noise_model);
  if (x3f_get_camf_float(x3f, "CaptureISO", &capture_iso) &&
      x3f_get_camf_float(x3f, "SensorISO", &sensor_iso) && sensor_iso > 0.0)
    iso_gain = (float)(capture_iso / sensor_iso);
  if (locality <= 0.0) locality = 1.0;
  edge_limit = (float)(4.0 / locality);
  edge_index_scale = 2047.0f / edge_limit;

  median = calloc(3 * pixels, sizeof(*median));
  raw_edge = calloc(pixels, sizeof(*raw_edge));
  if (!median || !raw_edge) goto failed;
  memset(coefficient, 0, pixels * sizeof(*coefficient));

  for (row = 1; row + 1 < image->rows; ++row) {
    for (col = 1; col + 1 < image->columns; ++col) {
      size_t i = (size_t)row * image->columns + col;
      for (channel = 0; channel < 3; ++channel) {
        const uint16_t *center = image->data + row * image->row_stride +
                                 col * image->channels;
        const uint16_t *left = center - image->channels;
        const uint16_t *right = center + image->channels;
        const uint16_t *top = center - image->row_stride;
        const uint16_t *bottom = center + image->row_stride;
        float scale = use_noise_model ? 1.0f : iso_gain;
        float bias = (float)x3f->merrill_intermediate_bias;
        float c = scale * ((center[channel] - bias) /
                           (float)sample_scale[channel]) + (float)lin_shift;
        float l = scale * ((left[channel] - bias) /
                           (float)sample_scale[channel]) + (float)lin_shift;
        float r = scale * ((right[channel] - bias) /
                           (float)sample_scale[channel]) + (float)lin_shift;
        float t = scale * ((top[channel] - bias) /
                           (float)sample_scale[channel]) + (float)lin_shift;
        float b = scale * ((bottom[channel] - bias) /
                           (float)sample_scale[channel]) + (float)lin_shift;
        median[3 * i + channel] = merrill_median5(c, l, r, t, b);
      }
    }
    for (channel = 0; channel < 3; ++channel) {
      size_t first = (size_t)row * image->columns;
      median[3 * first + channel] = median[3 * (first + 1) + channel];
      median[3 * (first + image->columns - 1) + channel] =
        median[3 * (first + image->columns - 2) + channel];
    }
  }

  for (row = 2; row + 2 < image->rows; ++row) {
    for (col = 1; col + 1 < image->columns; ++col) {
      size_t i = (size_t)row * image->columns + col;
      float horizontal = 0.0f, vertical = 0.0f;
      for (channel = 0; channel < 3; ++channel) {
        float center = median[3 * i + channel];
        float denominator = use_noise_model
          ? (float)(noise_model[channel] +
                    noise_model[3 + channel] * center)
          : center;
        float dx = (median[3 * (i - 1) + channel] -
                    median[3 * (i + 1) + channel]) / denominator;
        float dy = (median[3 * (i - image->columns) + channel] -
                    median[3 * (i + image->columns) + channel]) / denominator;
        float dx2 = dx * dx, dy2 = dy * dy;
        if (accumulate) {
          horizontal += dx2;
          vertical += dy2;
        } else {
          if (dx2 > horizontal) horizontal = dx2;
          if (dy2 > vertical) vertical = dy2;
        }
      }
      raw_edge[i] = sqrtf(0.5f * (horizontal + vertical));
    }
    raw_edge[(size_t)row * image->columns] =
      raw_edge[(size_t)row * image->columns + 1];
    raw_edge[(size_t)(row + 1) * image->columns - 1] =
      raw_edge[(size_t)(row + 1) * image->columns - 2];
  }

  for (row = 3; row + 3 < image->rows; ++row) {
    for (col = 1; col + 1 < image->columns; ++col) {
      size_t i = (size_t)row * image->columns + col;
      float edge = raw_edge[i];
      unsigned int index;
      if (smooth)
        edge = 0.2f * (edge + raw_edge[i - 1] + raw_edge[i + 1] +
                       raw_edge[i - image->columns] +
                       raw_edge[i + image->columns]);
      if (edge > edge_limit) edge = edge_limit;
      index = (unsigned int)(edge_index_scale * edge);
      if (index > 2047) index = 2047;
      coefficient[i] = decay * fmaxf(0.0f,
        (tanhf(-0.0019541f * index) + 1.0f - edge_strength) /
        (1.0f - edge_strength));
    }
    coefficient[(size_t)row * image->columns] =
      coefficient[(size_t)row * image->columns + 1];
    coefficient[(size_t)(row + 1) * image->columns - 1] =
      coefficient[(size_t)(row + 1) * image->columns - 2];
  }

  x3f_printf(DEBUG,
    "Merrill edge map: cross-median=1 (flag=%d) directional=no-op (flag=%d) "
    "smooth=%d accumulate=%d noise-model=%d shift=%g locality=%g\n",
    median_flag != 0, directional != 0, smooth != 0, accumulate != 0,
    use_noise_model != 0, lin_shift, locality);
  free(raw_edge);
  free(median);
  return 1;

failed:
  free(raw_edge);
  free(median);
  return 0;
}

/* Normalized forward/backward FIR used by Merrill LocalMean.  The coefficient
   map is generated from F20 ComputeEdgeMap and the recovered EdgeLUT (strength
   and decay are the ISO settings at indices 5 and 6). */
static int merrill_local_mean_exact(x3f_t *x3f, x3f_area16_t *image,
                                    const double *noise_model,
                                    const double *sample_scale,
                                    double bilateral_scale,
                                    float edge_strength, float decay) {
  size_t pixels = (size_t)image->rows * image->columns;
  float *coefficient = NULL, *horizontal = NULL;
  float *forward = NULL, *forward_norm = NULL;
  uint32_t row, col;
  unsigned int channel;
  coefficient = malloc(pixels * sizeof(*coefficient));
  horizontal = malloc(3 * pixels * sizeof(*horizontal));
  forward = malloc(3 * (image->columns > image->rows ? image->columns :
                        image->rows) * sizeof(*forward));
  forward_norm = malloc((image->columns > image->rows ? image->columns :
                         image->rows) * sizeof(*forward_norm));
  if (!coefficient || !horizontal || !forward || !forward_norm) goto failed;
  if (edge_strength >= 0.999f) edge_strength = 0.999f;
  if (edge_strength < 0.0f) edge_strength = 0.0f;
  if (getenv("FAST_SIGMA_DISABLE_EDGE_PREFILTERS") ||
      !merrill_edge_map_exact(x3f, image, noise_model, sample_scale,
                              edge_strength, decay, coefficient))
    for (row = 0; row < image->rows; ++row) {
      for (col = 0; col < image->columns; ++col) {
        const uint16_t *center = image->data + row * image->row_stride +
                                 col * image->channels;
        unsigned int edge = 0;
        if (col) {
          unsigned int value = merrill_range_index(center - image->channels,
            center, noise_model, sample_scale, bilateral_scale);
          if (value > edge) edge = value;
        }
        if (row) {
          unsigned int value = merrill_range_index(center - image->row_stride,
            center, noise_model, sample_scale, bilateral_scale);
          if (value > edge) edge = value;
        }
        if (edge > 2047) edge = 2047;
        coefficient[(size_t)row * image->columns + col] = decay * fmaxf(0.0f,
          (tanhf(-0.0019541f * edge) + 1.0f - edge_strength) /
          (1.0f - edge_strength));
      }
    }

  for (row = 0; row < image->rows; ++row) {
    const uint16_t *source = image->data + row * image->row_stride;
    size_t base = (size_t)row * image->columns;
    float backward[3], backward_norm = 1.0f;
    for (channel = 0; channel < 3; ++channel)
      forward[channel] = source[channel];
    forward_norm[0] = 1.0f;
    for (col = 1; col < image->columns; ++col) {
      float w = coefficient[base + col];
      for (channel = 0; channel < 3; ++channel)
        forward[3 * col + channel] = source[image->channels * col + channel] +
          w * forward[3 * (col - 1) + channel];
      forward_norm[col] = 1.0f + w * forward_norm[col - 1];
    }
    for (channel = 0; channel < 3; ++channel)
      backward[channel] = source[image->channels * (image->columns - 1) +
                                 channel];
    for (col = image->columns; col-- > 0;) {
      float norm;
      if (col + 1 < image->columns) {
        float w = coefficient[base + col + 1];
        backward_norm = 1.0f + w * backward_norm;
        for (channel = 0; channel < 3; ++channel)
          backward[channel] = source[image->channels * col + channel] +
            w * backward[channel];
      }
      norm = forward_norm[col] + backward_norm - 1.0f;
      for (channel = 0; channel < 3; ++channel)
        horizontal[3 * (base + col) + channel] =
          (forward[3 * col + channel] + backward[channel] -
           source[image->channels * col + channel]) / norm;
    }
  }

  for (col = 0; col < image->columns; ++col) {
    float backward[3], backward_norm = 1.0f;
    for (channel = 0; channel < 3; ++channel)
      forward[channel] = horizontal[3 * col + channel];
    forward_norm[0] = 1.0f;
    for (row = 1; row < image->rows; ++row) {
      size_t i = (size_t)row * image->columns + col;
      float w = coefficient[i];
      for (channel = 0; channel < 3; ++channel)
        forward[3 * row + channel] = horizontal[3 * i + channel] +
          w * forward[3 * (row - 1) + channel];
      forward_norm[row] = 1.0f + w * forward_norm[row - 1];
    }
    for (channel = 0; channel < 3; ++channel)
      backward[channel] = horizontal[3 *
        ((size_t)(image->rows - 1) * image->columns + col) + channel];
    for (row = image->rows; row-- > 0;) {
      size_t i = (size_t)row * image->columns + col;
      float norm;
      uint16_t *out = image->data + row * image->row_stride +
                      col * image->channels;
      if (row + 1 < image->rows) {
        float w = coefficient[i + image->columns];
        backward_norm = 1.0f + w * backward_norm;
        for (channel = 0; channel < 3; ++channel)
          backward[channel] = horizontal[3 * i + channel] +
            w * backward[channel];
      }
      norm = forward_norm[row] + backward_norm - 1.0f;
      for (channel = 0; channel < 3; ++channel) {
        long value = lrintf((forward[3 * row + channel] + backward[channel] -
          horizontal[3 * i + channel]) / norm);
        out[channel] = (uint16_t)(value < 0 ? 0 :
                                  value > 16383 ? 16383 : value);
      }
    }
  }
  free(forward_norm); free(forward); free(horizontal); free(coefficient);
  return 1;
failed:
  free(forward_norm); free(forward); free(horizontal); free(coefficient);
  return 0;
}

static uint16_t filter_step_u16(uint16_t value, uint16_t neighbor,
                                float weight) {
  long filtered = lrintf(value + weight * ((float)neighbor - value));
  return (uint16_t)(filtered < 0 ? 0 : filtered > 16383 ? 16383 : filtered);
}

static void filter_chroma_rows(int16_t *u, int16_t *v, uint16_t *t,
                               const x3f_area16_t *image,
                               const float *weight,
                               float chroma_edge_weight,
                               const double *noise_model,
                               const double *sample_scale,
                               double bilateral_scale) {
  uint32_t row, col;
  for (row = 0; row < image->rows; ++row) {
    size_t base = (size_t)row * image->columns;
    for (col = 1; col < image->columns; ++col) {
      size_t i = base + col;
      uint16_t y0 = image->data[image->row_stride * row
                                + image->channels * (col - 1) + 2];
      uint16_t y1 = image->data[image->row_stride * row
                                + image->channels * col + 2];
      const uint16_t *left = image->data + image->row_stride * row
                             + image->channels * (col - 1);
      const uint16_t *right = left + image->channels;
      unsigned int delta = noise_model
        ? merrill_range_index(left, right, noise_model, sample_scale,
                              bilateral_scale)
        : residual_edge(y0, y1, u[i - 1], u[i], v[i - 1], v[i],
                        chroma_edge_weight);
      float w = delta > 2047 ? 0.0f : weight[delta];
      u[i] = filter_step(u[i], u[i - 1], w);
      v[i] = filter_step(v[i], v[i - 1], w);
      if (t) t[i] = filter_step_u16(t[i], t[i - 1], w);
    }
    for (col = image->columns - 1; col > 0; --col) {
      size_t i = base + col - 1;
      uint16_t y0 = image->data[image->row_stride * row
                                + image->channels * (col - 1) + 2];
      uint16_t y1 = image->data[image->row_stride * row
                                + image->channels * col + 2];
      const uint16_t *left = image->data + image->row_stride * row
                             + image->channels * (col - 1);
      const uint16_t *right = left + image->channels;
      unsigned int delta = noise_model
        ? merrill_range_index(left, right, noise_model, sample_scale,
                              bilateral_scale)
        : residual_edge(y0, y1, u[i], u[i + 1], v[i], v[i + 1],
                        chroma_edge_weight);
      float w = delta > 2047 ? 0.0f : weight[delta];
      u[i] = filter_step(u[i], u[i + 1], w);
      v[i] = filter_step(v[i], v[i + 1], w);
      if (t) t[i] = filter_step_u16(t[i], t[i + 1], w);
    }
  }
}

static void filter_chroma_columns(int16_t *u, int16_t *v, uint16_t *t,
                                  const x3f_area16_t *image,
                                  const float *weight,
                                  float chroma_edge_weight,
                                  const double *noise_model,
                                  const double *sample_scale,
                                  double bilateral_scale) {
  uint32_t row, col;
  size_t columns = image->columns;
  for (col = 0; col < image->columns; ++col) {
    for (row = 1; row < image->rows; ++row) {
      size_t i = (size_t)row * columns + col;
      uint16_t y0 = image->data[image->row_stride * (row - 1)
                                + image->channels * col + 2];
      uint16_t y1 = image->data[image->row_stride * row
                                + image->channels * col + 2];
      const uint16_t *top = image->data + image->row_stride * (row - 1)
                            + image->channels * col;
      const uint16_t *bottom = top + image->row_stride;
      unsigned int delta = noise_model
        ? merrill_range_index(top, bottom, noise_model, sample_scale,
                              bilateral_scale)
        : residual_edge(y0, y1, u[i - columns], u[i],
                        v[i - columns], v[i], chroma_edge_weight);
      float w = delta > 2047 ? 0.0f : weight[delta];
      u[i] = filter_step(u[i], u[i - columns], w);
      v[i] = filter_step(v[i], v[i - columns], w);
      if (t) t[i] = filter_step_u16(t[i], t[i - columns], w);
    }
    for (row = image->rows - 1; row > 0; --row) {
      size_t i = (size_t)(row - 1) * columns + col;
      uint16_t y0 = image->data[image->row_stride * (row - 1)
                                + image->channels * col + 2];
      uint16_t y1 = image->data[image->row_stride * row
                                + image->channels * col + 2];
      const uint16_t *top = image->data + image->row_stride * (row - 1)
                            + image->channels * col;
      const uint16_t *bottom = top + image->row_stride;
      unsigned int delta = noise_model
        ? merrill_range_index(top, bottom, noise_model, sample_scale,
                              bilateral_scale)
        : residual_edge(y0, y1, u[i], u[i + columns],
                        v[i], v[i + columns], chroma_edge_weight);
      float w = delta > 2047 ? 0.0f : weight[delta];
      u[i] = filter_step(u[i], u[i + columns], w);
      v[i] = filter_step(v[i], v[i + columns], w);
      if (t) t[i] = filter_step_u16(t[i], t[i + columns], w);
    }
  }
}

/* Perona-Malik diffusion used by Photo Pro's residual path.  The native
   routine uses the same four-neighbour conduction function and lambda=.25;
   this scalar form deliberately updates through a second buffer so row order
   cannot bias the result. */
static void diffuse_residual(const float *source, float *destination,
                             uint32_t rows, uint32_t columns,
                             float noise, float lambda) {
  uint32_t row, col;
  float inverse_noise2 = noise > 0.0f ? 1.0f / (noise * noise) : 0.0f;
  memcpy(destination, source, (size_t)rows * columns * sizeof(*source));
  for (row = 1; row + 1 < rows; ++row) {
    for (col = 1; col + 1 < columns; ++col) {
      size_t i = (size_t)row * columns + col;
      float center = source[i];
      float delta[4] = {
        source[i - columns] - center, source[i + columns] - center,
        source[i - 1] - center, source[i + 1] - center
      };
      float update = 0.0f;
      unsigned int direction;
      for (direction = 0; direction < 4; ++direction)
        update += delta[direction] /
          (1.0f + delta[direction] * delta[direction] * inverse_noise2);
      destination[i] = center + lambda * update;
    }
  }
}

/* OSFFilter removes only strict one-pixel extrema.  It is intentionally much
   less destructive than an unconditional median: non-extreme texture and
   edges pass through bit-for-bit. */
static void suppress_residual_extrema(const float *source, float *destination,
                                      uint32_t rows, uint32_t columns) {
  uint32_t row, col;
  memcpy(destination, source, (size_t)rows * columns * sizeof(*source));
  for (row = 1; row + 1 < rows; ++row) {
    for (col = 1; col + 1 < columns; ++col) {
      size_t i = (size_t)row * columns + col;
      float center = source[i];
      float low = source[i - columns], high = low;
      float neighbors[3] = {
        source[i + columns], source[i - 1], source[i + 1]
      };
      unsigned int n;
      for (n = 0; n < 3; ++n) {
        if (neighbors[n] < low) low = neighbors[n];
        if (neighbors[n] > high) high = neighbors[n];
      }
      if (center < low) destination[i] = low;
      else if (center > high) destination[i] = high;
    }
  }
}

static float residual_strength_curve(float ratio, float strength) {
  if (ratio <= 0.000001f) ratio = 0.000001f;
  if (ratio >= 1.0f) return 1.0f + (ratio - 1.0f) * strength;
  return 1.0f / (1.0f + (1.0f / ratio - 1.0f) * strength);
}

/* At ISO 100 ResidualRCPS_Blending is zero.  Higher Merrill ISOs enable it:
   Photo Pro marks locally near-unity areas with two radius-four min/max
   passes, permutes row/column samples there, and blends the decorrelated map
   back.  A fixed integer hash reproduces that operation deterministically. */
static void suppress_row_column_pattern(float *gain, float *scratch,
                                        uint32_t rows, uint32_t columns,
                                        unsigned int radius,
                                        float map_threshold,
                                        float sample_threshold,
                                        float blending) {
  uint32_t row, col;
  if (blending <= 0.0f || radius == 0) return;
  memcpy(scratch, gain, (size_t)rows * columns * sizeof(*gain));
  for (row = radius; row + radius < rows; ++row) {
    for (col = radius; col + radius < columns; ++col) {
      size_t i = (size_t)row * columns + col;
      float low = scratch[i], high = scratch[i];
      int dy, dx;
      unsigned int hash, span, ox, oy;
      float replacement;
      for (dy = -(int)radius; dy <= (int)radius; ++dy)
        for (dx = -(int)radius; dx <= (int)radius; ++dx) {
          float value = scratch[(size_t)(row + dy) * columns + col + dx];
          if (value < low) low = value;
          if (value > high) high = value;
        }
      if (fabsf(low - 1.0f) >= map_threshold ||
          fabsf(high - 1.0f) >= map_threshold ||
          fabsf(scratch[i] - 1.0f) >= sample_threshold)
        continue;
      hash = 0x9e3779b9u * (row + 1u) ^ 0x85ebca6bu * (col + 1u);
      span = 2 * radius + 1;
      ox = hash % span;
      oy = (hash >> 16) % span;
      replacement = 0.5f *
        (scratch[(size_t)row * columns + col + ox - radius] +
         scratch[(size_t)(row + oy - radius) * columns + col]);
      gain[i] = blending * replacement + (1.0f - blending) * scratch[i];
    }
  }
}

float x3f_merrill_residual_gain_at(const x3f_t *x3f,
                                   const uint16_t *pixel,
                                   unsigned int channel) {
  ptrdiff_t offset;
  uint32_t row, column;
  if (!x3f || !x3f->merrill_residual_gain ||
      !x3f->merrill_residual_origin || !pixel)
    return 1.0f;
  offset = pixel - x3f->merrill_residual_origin;
  if (offset < 0 || x3f->merrill_residual_row_stride == 0 ||
      x3f->merrill_residual_channels == 0)
    return 1.0f;
  row = (uint32_t)offset / x3f->merrill_residual_row_stride;
  column = ((uint32_t)offset % x3f->merrill_residual_row_stride) /
    x3f->merrill_residual_channels;
  if (row >= x3f->merrill_residual_rows ||
      column >= x3f->merrill_residual_columns)
    return 1.0f;
  if (x3f->merrill_residual_gain_channels == 0)
    return 1.0f;
  if (channel >= x3f->merrill_residual_gain_channels) channel = 0;
  return x3f->merrill_residual_gain[
    ((size_t)row * x3f->merrill_residual_columns + column) *
    x3f->merrill_residual_gain_channels + channel];
}

void x3f_denoise(x3f_t *x3f, x3f_area16_t *image, x3f_denoise_type_t type) {
  float spatial_weight = 0.80f;
  float edge_scale = 82.0f;
  float chroma_edge_weight = 0.0f;
  float residual_mix = 0.0f;
  float residual_color_mix = 0.0f;
  float residual_strength = 2.0f;
  float residual_epsilon = 5.0f;
  float residual_noise = 0.025f;
  float residual_map_threshold = 0.025f;
  float residual_sample_threshold = 0.025f;
  float residual_rcps_blending = 0.0f;
  unsigned int residual_radius = 4;
  unsigned int passes = 1;
  size_t pixels, i;
  int16_t *u, *v;
  uint16_t *t = NULL;
  uint16_t *original_rgb = NULL;
  float *gain_map = NULL, *scratch = NULL, *color_gain = NULL;
  float *weight;
  const char *setting;
  double noise_model_storage[9], sample_scale[3];
  double iso_settings[13] = {
    100.0, 1.0, 1.0, 10.0, 0.005, 0.7, 0.555,
    4.0, 1.0, 0.8, 0.0, 2.0, 1.0
  };
  double additional_settings[13];
  double intensity_weight[3] = {1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0};
  const double *noise_model = NULL;
  double bilateral_scale = 1.0;
  int native_residual, exact_spatial;
  unsigned int bilateral_radius = 10, subsample_kernel = 1;

  if (type != X3F_DENOISE_F20 || !image || image->channels < 3
      || image->columns < 2 || image->rows < 2)
    return;
  pixels = (size_t)image->columns * image->rows;
  native_residual = !getenv("FAST_SIGMA_DISABLE_RESIDUAL_PIPELINE");
  exact_spatial = !getenv("FAST_SIGMA_USE_RECURSIVE_SPATIAL");
  if (x3f) {
    free(x3f->merrill_residual_gain);
    x3f->merrill_residual_gain = NULL;
    x3f->merrill_residual_origin = NULL;
    x3f->merrill_residual_rows = 0;
    x3f->merrill_residual_columns = 0;
    x3f->merrill_residual_gain_channels = 0;
  }

  if (native_residual && x3f &&
      merrill_interpolate_noise_row(x3f, "ISONoiseSettings", iso_settings)) {
    residual_mix = (float)iso_settings[9];
    residual_color_mix = (float)iso_settings[10];
    residual_strength = (float)iso_settings[11];
    bilateral_radius = (unsigned int)fmax(1.0, iso_settings[3]);
  }
  if (x3f) {
    unsigned int camf_subsample;
    if (x3f_get_camf_unsigned(x3f, "SubSampleKernel", &camf_subsample))
      subsample_kernel = camf_subsample != 0;
  }
  if (native_residual && x3f &&
      merrill_interpolate_noise_row(x3f, "AdditionalISONoiseSettings",
                                    additional_settings)) {
    residual_map_threshold = (float)additional_settings[1];
    residual_sample_threshold = (float)additional_settings[2];
    residual_rcps_blending = (float)additional_settings[3];
    if (additional_settings[1] > 0.0)
      residual_noise = (float)additional_settings[1];
  }

  /* Developer-only controls used to fit the independently reconstructed
     residual stage against paired Photo Pro exports. */
  setting = getenv("FAST_SIGMA_DENOISE_WEIGHT");
  if (setting) spatial_weight = strtof(setting, NULL);
  setting = getenv("FAST_SIGMA_DENOISE_EDGE_SCALE");
  if (setting) edge_scale = strtof(setting, NULL);
  setting = getenv("FAST_SIGMA_DENOISE_CHROMA_EDGE");
  if (setting) chroma_edge_weight = strtof(setting, NULL);
  setting = getenv("FAST_SIGMA_DENOISE_RESIDUAL_MIX");
  if (setting) residual_mix = strtof(setting, NULL);
  setting = getenv("FAST_SIGMA_DENOISE_RESIDUAL_STRENGTH");
  if (setting) residual_strength = strtof(setting, NULL);
  setting = getenv("FAST_SIGMA_DENOISE_RESIDUAL_COLOR_MIX");
  if (setting) residual_color_mix = strtof(setting, NULL);
  setting = getenv("FAST_SIGMA_DENOISE_PASSES");
  if (setting) passes = (unsigned int)strtoul(setting, NULL, 10);
  if (spatial_weight < 0.0f) spatial_weight = 0.0f;
  if (spatial_weight > 0.999f) spatial_weight = 0.999f;
  if (edge_scale < 1.0f) edge_scale = 1.0f;
  if (chroma_edge_weight < 0.0f) chroma_edge_weight = 0.0f;
  if (residual_mix < 0.0f) residual_mix = 0.0f;
  if (residual_mix > 1.0f) residual_mix = 1.0f;
  if (residual_strength < 0.0f) residual_strength = 0.0f;
  if (residual_color_mix < 0.0f) residual_color_mix = 0.0f;
  if (residual_color_mix > 1.0f) residual_color_mix = 1.0f;
  if (passes < 1) passes = 1;
  if (passes > 8) passes = 8;

  if (!getenv("FAST_SIGMA_DISABLE_NATIVE_RANGE") && x3f) {
    double gain[3], max_gain = 0.0, weight_sum = 0.0;
    if (x3f_get_camf_matrix(x3f, "ColorNoiseModel", 3, 3, 0, M_FLOAT,
                            noise_model_storage) &&
        x3f_get_gain(x3f, x3f_get_wb(x3f), gain)) {
      unsigned int channel;
      for (channel = 0; channel < 3; ++channel)
        if (gain[channel] > max_gain) max_gain = gain[channel];
      if (max_gain > 0.0) {
        for (channel = 0; channel < 3; ++channel) {
          sample_scale[channel] = 4.0 / max_gain;
          if (gain[channel] > 0.0) {
            intensity_weight[channel] = 1.0 / gain[channel];
            weight_sum += intensity_weight[channel];
          }
        }
        if (weight_sum > 0.0)
          for (channel = 0; channel < 3; ++channel)
            intensity_weight[channel] /= weight_sum;
        bilateral_scale = 2047.0 * merrill_noise_scaling(x3f) / 8.0;
        noise_model = noise_model_storage;
      }
    }
  }
  if (exact_spatial && !noise_model) exact_spatial = 0;

  u = exact_spatial ? NULL : malloc(pixels * sizeof(*u));
  v = exact_spatial ? NULL : malloc(pixels * sizeof(*v));
  if (native_residual) {
    t = malloc(pixels * sizeof(*t));
    gain_map = malloc(pixels * sizeof(*gain_map));
    if (residual_color_mix > 0.0f)
      original_rgb = malloc(3 * pixels * sizeof(*original_rgb));
  }
  weight = malloc(2048 * sizeof(*weight));
  if ((!exact_spatial && (!u || !v)) || !weight ||
      (native_residual && (!t || !gain_map ||
       (residual_color_mix > 0.0f && !original_rgb))))
    goto done;

  for (i = 0; i < 2048; ++i)
    weight[i] = spatial_weight * expf(-(float)i /
      (noise_model ? 256.0f : edge_scale));
  for (i = 0; i < pixels; ++i) {
    uint32_t row = (uint32_t)(i / image->columns);
    uint32_t col = (uint32_t)(i - (size_t)row * image->columns);
    const uint16_t *pixel = image->data + image->row_stride * row
                            + image->channels * col;
    float b = pixel[0], m = pixel[1], top_sample = pixel[2];
    if (!exact_spatial) {
      u[i] = (int16_t)(2 * (b - top_sample));
      v[i] = (int16_t)(b - 2 * m + top_sample);
    }
    if (native_residual) {
      /* Native intensity coefficients undo the unequal Foveon plane gains
         before forming a mono residual. */
      gain_map[i] = (float)(intensity_weight[0] * b +
                            intensity_weight[1] * m +
                            intensity_weight[2] * top_sample);
      if (original_rgb) {
        original_rgb[3 * i] = pixel[0];
        original_rgb[3 * i + 1] = pixel[1];
        original_rgb[3 * i + 2] = pixel[2];
      }
    }
  }

  if (native_residual)
    for (i = 0; i < pixels; ++i) {
      uint32_t row = (uint32_t)(i / image->columns);
      uint32_t col = (uint32_t)(i - (size_t)row * image->columns);
      const uint16_t *pixel = image->data + image->row_stride * row
                              + image->channels * col;
      t[i] = pixel[2];
    }

  if (exact_spatial && noise_model) {
    if (merrill_bilateral_rgb_exact(image, noise_model, sample_scale,
                                    bilateral_scale, bilateral_radius,
                                    subsample_kernel)) {
      float edge_strength = (float)iso_settings[5];
      float local_mean_decay = (float)iso_settings[6];
      merrill_local_mean_exact(x3f, image, noise_model, sample_scale,
                               bilateral_scale, edge_strength,
                               local_mean_decay);
      x3f_printf(DEBUG,
        "Merrill spatial: exact %ux%u bilateral step=%u + local mean\n",
        2 * bilateral_radius + 1, 2 * bilateral_radius + 1,
        subsample_kernel ? 2 : 1);
    } else {
      exact_spatial = 0;
      u = malloc(pixels * sizeof(*u));
      v = malloc(pixels * sizeof(*v));
      if (!u || !v) goto done;
      for (i = 0; i < pixels; ++i) {
        uint32_t row = (uint32_t)(i / image->columns);
        uint32_t col = (uint32_t)(i - (size_t)row * image->columns);
        const uint16_t *pixel = image->data + image->row_stride * row +
                                image->channels * col;
        u[i] = (int16_t)(2 * ((float)pixel[0] - pixel[2]));
        v[i] = (int16_t)((float)pixel[0] - 2.0f * pixel[1] + pixel[2]);
      }
    }
  }
  if (!exact_spatial) {
    for (i = 0; i < passes; ++i) {
      filter_chroma_rows(u, v, t, image, weight, chroma_edge_weight,
                         noise_model, sample_scale, bilateral_scale);
      filter_chroma_columns(u, v, t, image, weight, chroma_edge_weight,
                            noise_model, sample_scale, bilateral_scale);
    }

    for (i = 0; i < pixels; ++i) {
    uint32_t row = (uint32_t)(i / image->columns);
    uint32_t col = (uint32_t)(i - (size_t)row * image->columns);
    uint16_t *pixel = image->data + image->row_stride * row
                      + image->channels * col;
    float top = native_residual ? t[i] : pixel[2];
    float original_u = 2.0f * ((float)pixel[0] - pixel[2]);
    float original_v = (float)pixel[0] - 2.0f * pixel[1] + pixel[2];
    float mixed_u = u[i] + residual_mix * (original_u - u[i]);
    float mixed_v = v[i] + residual_mix * (original_v - v[i]);
    long b, m, top_value;
    if (native_residual) {
      mixed_u = u[i];
      mixed_v = v[i];
    }
    b = lrintf(top + 0.5f * mixed_u);
    m = lrintf(top + 0.25f * mixed_u - 0.5f * mixed_v);
    top_value = lrintf(top);
    pixel[0] = (uint16_t)(b < 0 ? 0 : b > 16383 ? 16383 : b);
    pixel[1] = (uint16_t)(m < 0 ? 0 : m > 16383 ? 16383 : m);
    if (native_residual)
      pixel[2] = (uint16_t)(top_value < 0 ? 0 : top_value > 16383
                            ? 16383 : top_value);
    }
  }

  if (native_residual) {
    free(v);
    v = NULL;
    free(u);
    u = NULL;
    scratch = malloc(pixels * sizeof(*scratch));
    if (!scratch) goto done;
    for (i = 0; i < pixels; ++i) {
      uint32_t row = (uint32_t)(i / image->columns);
      uint32_t col = (uint32_t)(i - (size_t)row * image->columns);
      const uint16_t *pixel = image->data + image->row_stride * row
                              + image->channels * col;
      float base = (float)(intensity_weight[0] * pixel[0] +
                           intensity_weight[1] * pixel[1] +
                           intensity_weight[2] * pixel[2]);
      float ratio = (gain_map[i] + residual_epsilon) /
                    (base + residual_epsilon);
      if (ratio < 0.0f) ratio = 0.0f;
      if (ratio > 7.999f) ratio = 7.999f;
      t[i] = (uint16_t)lrintf(ratio * 8192.0f);
      gain_map[i] = ratio;
    }

    diffuse_residual(gain_map, scratch, image->rows, image->columns,
                     residual_noise, 0.25f);
    diffuse_residual(scratch, gain_map, image->rows, image->columns,
                     2.0f, 0.25f);
    suppress_residual_extrema(gain_map, scratch,
                              image->rows, image->columns);
    suppress_row_column_pattern(scratch, gain_map,
                                image->rows, image->columns,
                                residual_radius, residual_map_threshold,
                                residual_sample_threshold,
                                residual_rcps_blending);

    if (residual_color_mix > 0.0f) {
      color_gain = malloc(3 * pixels * sizeof(*color_gain));
      if (!color_gain) goto done;
    }
    for (i = 0; i < pixels; ++i) {
      float raw_ratio = t[i] / 8192.0f;
      float mono_ratio = (1.0f - residual_mix) * raw_ratio +
                         residual_mix * scratch[i];
      if (color_gain) {
        uint32_t row = (uint32_t)(i / image->columns);
        uint32_t col = (uint32_t)(i - (size_t)row * image->columns);
        const uint16_t *base = image->data + image->row_stride * row
                               + image->channels * col;
        unsigned int channel;
        for (channel = 0; channel < 3; ++channel) {
          float numerator = 3.0f * original_rgb[3 * i + channel] *
                            (float)intensity_weight[channel] +
                            residual_epsilon;
          float denominator = 3.0f * base[channel] *
                              (float)intensity_weight[channel] +
                              residual_epsilon;
          float channel_ratio = numerator / denominator;
          float ratio = residual_color_mix * channel_ratio +
                        (1.0f - residual_color_mix) * mono_ratio;
          color_gain[3 * i + channel] =
            residual_strength_curve(ratio, residual_strength);
        }
      } else {
        scratch[i] = residual_strength_curve(mono_ratio,
                                             residual_strength);
      }
    }
    x3f->merrill_residual_gain = color_gain ? color_gain : scratch;
    x3f->merrill_residual_origin = image->data;
    x3f->merrill_residual_rows = image->rows;
    x3f->merrill_residual_columns = image->columns;
    x3f->merrill_residual_gain_channels = color_gain ? 3 : 1;
    x3f->merrill_residual_channels = image->channels;
    x3f->merrill_residual_row_stride = image->row_stride;
    if (color_gain) color_gain = NULL;
    else scratch = NULL;
    x3f_printf(DEBUG,
      "Merrill residual: mix=%g color=%g strength=%g noise=%g RCPS=%g\n",
      residual_mix, residual_color_mix, residual_strength,
      residual_noise, residual_rcps_blending);
  }

done:
  free(color_gain);
  free(original_rgb);
  free(scratch);
  free(gain_map);
  free(t);
  free(weight);
  free(v);
  free(u);
}

void x3f_set_use_opencl(int flag) { (void)flag; }

/*
 * Quattro has a full-resolution top plane and half-resolution lower planes.
 * Expand the latter with bilinear interpolation, then restore the measured
 * top-plane sample.  This avoids OpenCV and keeps the converter self-contained.
 */
void x3f_expand_quattro(x3f_area16_t *image, x3f_area16_t *active,
                        x3f_area16_t *qtop, x3f_area16_t *expanded,
                        x3f_area16_t *active_exp) {
  uint32_t y, x, c;
  (void)active;
  (void)active_exp;

  for (y = 0; y < expanded->rows; ++y) {
    const double sy = (expanded->rows > 1)
      ? (double)y * (image->rows - 1) / (expanded->rows - 1) : 0.0;
    const uint32_t y0 = (uint32_t)sy;
    const uint32_t y1 = y0 + 1 < image->rows ? y0 + 1 : y0;
    const double fy = sy - y0;
    for (x = 0; x < expanded->columns; ++x) {
      const double sx = (expanded->columns > 1)
        ? (double)x * (image->columns - 1) / (expanded->columns - 1) : 0.0;
      const uint32_t x0 = (uint32_t)sx;
      const uint32_t x1 = x0 + 1 < image->columns ? x0 + 1 : x0;
      const double fx = sx - x0;
      uint16_t *dst = expanded->data + expanded->row_stride * y
                      + expanded->channels * x;

      for (c = 0; c < 3; ++c) {
        const uint16_t *p00 = image->data + image->row_stride * y0
                              + image->channels * x0;
        const uint16_t *p01 = image->data + image->row_stride * y0
                              + image->channels * x1;
        const uint16_t *p10 = image->data + image->row_stride * y1
                              + image->channels * x0;
        const uint16_t *p11 = image->data + image->row_stride * y1
                              + image->channels * x1;
        const double a = p00[c] + (p01[c] - p00[c]) * fx;
        const double b = p10[c] + (p11[c] - p10[c]) * fx;
        dst[c] = (uint16_t)(a + (b - a) * fy + 0.5);
      }
      dst[2] = qtop->data[qtop->row_stride * y + qtop->channels * x];
    }
  }
}
