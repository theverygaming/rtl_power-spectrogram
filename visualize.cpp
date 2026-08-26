#include <cstdlib>
#include <cstdio>
#include <vector>
#include <unordered_map>
#include <ctime>
#include <cstddef>
#include <memory>
#include <png.h>
#include <cerrno>
#include <sys/stat.h>
#include <sstream>
#include <limits>
#include <iomanip>
#include <fstream>
#include <algorithm>
#include <cmath>

#define MAX_OUTPUT_LINES 100

//#define DEBUG_LINES 1

#define EXIT_ERROR(fmt, ...) \
    do { \
        fprintf(stderr, "error: " fmt "\n", ##__VA_ARGS__); \
        exit(1); \
    } while (0)

struct image_meta {
    time_t ts_start;
    size_t height;
    std::string fname;
};

struct range {
    unsigned long long flow; // freq low in Hz
    unsigned long long fhigh; // freq high in Hz
    double fstep; // step in Hz
    size_t width;
    time_t ts_start;
    std::vector<float> samples;
    std::vector<struct image_meta> imgmetas;
};

static png_bytep *makepngrows(struct range *r) {
    size_t height = r->samples.size() / r->width;

    png_bytep *row_pointers = (png_bytep *)malloc(sizeof(png_bytep) * height);
    if (row_pointers == nullptr) {
        return nullptr;
    }

    size_t row_bytes = r->width * 3; // RGB

    uint8_t *rowdata = (uint8_t *)malloc(row_bytes * height);
    if (rowdata == nullptr) {
        free(row_pointers);
        return nullptr;
    }

    for (size_t i = 0; i < height; i++) {
        row_pointers[i] = rowdata + (i * row_bytes);
        for (size_t j = 0; j < r->width; j++) {
            float db = r->samples[(i * r->width) + j];
            float min = -40;
            float max = 0;
            float val = std::clamp((db - min) / (max - min), 0.0f, 1.0f);

            // colormap values from: https://github.com/AlexandreRouma/SDRPlusPlus/blob/8c9f5ee8fe405775bfcd62c8c8f8c0fc928a64af/root/res/colormaps/websdr.json#L1-L11
            size_t n_steps = 5;
            uint8_t colsteps_r[] = {0x00, 0x00, 0xFF, 0xFF, 0xFF};
            uint8_t colsteps_g[] = {0x00, 0x00, 0x00, 0xFF, 0xFF};
            uint8_t colsteps_b[] = {0x00, 0x50, 0xFF, 0x50, 0xFF};
            float step_pos = val / (1.0 / (n_steps - 1));
            size_t idx_l = std::floor(step_pos);
            size_t idx_h = std::min(idx_l + 1, n_steps - 1);
            float step_scale = step_pos - idx_l;

            row_pointers[i][(j * 3) + 0] = ((1.0f - step_scale) * colsteps_r[idx_l]) + (step_scale * colsteps_r[idx_h]); // R
            row_pointers[i][(j * 3) + 1] = ((1.0f - step_scale) * colsteps_g[idx_l]) + (step_scale * colsteps_g[idx_h]); // G
            row_pointers[i][(j * 3) + 2] = ((1.0f - step_scale) * colsteps_b[idx_l]) + (step_scale * colsteps_b[idx_h]); // B
        }
    }

    return row_pointers;
}

static void free_pngrows(struct range *r, png_bytep *ptr) {
    free(ptr[0]);
    free(ptr);
}

static int write_png(struct range *r, const char *fname) {
    // prepare libpng stuff, this is basically straight from the libpng manual :P
    FILE *fp = fopen(fname, "wb");
    if (!fp) {
        return -1;
    }

    png_structp png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) {
        return -1;
    }

    png_infop info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_write_struct(&png_ptr, (png_infopp)NULL);
        return -1;
    }

    png_bytep* pngrows = makepngrows(r);
    if (pngrows == nullptr) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        fclose(fp);
        return -1;
    }

    if (setjmp(png_jmpbuf(png_ptr))) {
        png_destroy_write_struct(&png_ptr, &info_ptr);
        fclose(fp);
        free_pngrows(r, pngrows);
        return -1;
    }

    png_init_io(png_ptr, fp);

    png_set_IHDR(png_ptr,
        info_ptr,
        r->width, // width
        r->samples.size() / r->width, // height
        8, // bit depth
        PNG_COLOR_TYPE_RGB, // color type
        PNG_INTERLACE_NONE, // interlace type
        PNG_COMPRESSION_TYPE_DEFAULT,  // compression type
        PNG_FILTER_TYPE_DEFAULT // filter method
    );

    png_write_info(png_ptr, info_ptr);
    png_write_image(png_ptr, pngrows);
    png_write_end(png_ptr, NULL);

    png_destroy_write_struct(&png_ptr, &info_ptr);
    fclose(fp);
    free_pngrows(r, pngrows);

    return 0;
}

static int dump_png(struct range *r) {
    std::stringstream fname_out_stream;
    fname_out_stream << std::defaultfloat << std::setprecision(std::numeric_limits<float>::max_digits10);
    fname_out_stream << "spec-" << r->ts_start << "-" << r->flow << "-" << r->fhigh << "-" << r->fstep << ".png";
    std::string fname_out = fname_out_stream.str();

    if (write_png(r, ("out/" + fname_out).c_str()) != 0) {
        return -1;
    }

    r->imgmetas.push_back(image_meta {
        .ts_start = r->ts_start,
        .height = r->samples.size() / r->width,
        .fname = fname_out,
    });

    r->samples.clear();
    return 0;
}

static std::unordered_map<size_t, std::shared_ptr<struct range>> ranges;

static size_t hash_freqrange(unsigned long long flow, unsigned long long fhigh, double fstep) {
    return std::hash<unsigned long long>{}(flow) + std::hash<unsigned long long>{}(fhigh) + std::hash<double>{}(fstep);
}

static int add_line(time_t ts, unsigned long long flow, unsigned long long fhigh, double fstep, std::vector<float> *samples) {
    size_t freqrange_hash = hash_freqrange(flow, fhigh, fstep);
    auto it = ranges.find(freqrange_hash);
    if (it == ranges.end()) {
        it = ranges.insert({freqrange_hash, std::make_shared<struct range>(range {
            .flow = flow,
            .fhigh = fhigh,
            .fstep = fstep,
            .width = samples->size(),
            .samples = {},
        })}).first;
    }
    std::shared_ptr<struct range> val = it->second;
    if (samples->size() != val->width) {
        return -1;
    }

    if (val->samples.size() == 0) {
        val->ts_start = ts;
    }

    val->samples.reserve(val->samples.size() + samples->size());
    val->samples.insert(val->samples.end(), samples->begin(), samples->end());

    if ((val->samples.size() / val->width) >= MAX_OUTPUT_LINES) {
        if (dump_png(val.get()) != 0) {
            return -1;
        }
    }

    return 0;
}

int main(int argc, char **argv) {
    printf("hi!\n");

    if (mkdir("out/", 0755) != 0) {
        if (errno != EEXIST) {
            EXIT_ERROR("could not create output directory");
        }
    }

    unsigned long long linecounter = 0;
    std::vector<float> samples;
    while (!feof(stdin)) {
        // metadata
        char ts_str[10 + 1 + 8 + 1]; // 10 date, 8 time format YYYY-MM-DD_HH:MM:SS
        unsigned long long flow; // freq low in Hz
        unsigned long long fhigh; // freq high in Hz
        double fstep; // step in Hz
        unsigned int nsamps; // number of samples, but I have NO IDEA what this is for lol
        int res = fscanf(stdin, "%10s,%8s,%llu,%llu,%lf,%u", ts_str, &ts_str[11], &flow, &fhigh, &fstep, &nsamps);
        ts_str[10] = '_';
        if (res == EOF) {
            break;
        } else if (res != 6) {
            EXIT_ERROR("couldn't parse metadata (line %llu)", linecounter + 1);
        }
        struct tm tm;
        if (strptime(ts_str, "%Y-%m-%d_%H:%M:%S", &tm) == nullptr) {
            EXIT_ERROR("couldn't time from metadata (line %llu)", linecounter + 1);
        }
        time_t ts = mktime(&tm);
#ifdef DEBUG_LINES
        fprintf(stderr, "DBG: ts: %s low: %lluHz high: %lluHz step: %lfHz nsamps: %u\n", ts_str, flow, fhigh, fstep, nsamps);
#endif

        // samples
        int c;
        while ((c = fgetc(stdin)) != EOF && c != '\n') {
            if (c != ',') {
                EXIT_ERROR("could not parse samples, expected comma (line %llu)", linecounter + 1);
            }
            float value;
            if (fscanf(stdin, "%f", &value) != 1) {
                EXIT_ERROR("could not parse sample value (line %llu)", linecounter + 1);
            }
#ifdef DEBUG_LINES
            fprintf(stderr, "DBG: sample value: %f\n", value);
#endif
            samples.push_back(value);
        }
        if (add_line(ts, flow, fhigh, fstep, &samples) != 0) {
            EXIT_ERROR("error adding line (line %llu)", linecounter + 1);
        }
        samples.clear();

        linecounter++;
    }
    printf("EOF! lines: %llu\n", linecounter);

    // write remaining images
    for (auto it = ranges.begin(); it != ranges.end(); it++) {
        if (dump_png(it->second.get()) != 0) {
            EXIT_ERROR("could not dump PNG");
        }   
    }

    // write metadata
    std::stringstream metajson;
    metajson << std::defaultfloat << std::setprecision(std::numeric_limits<float>::max_digits10);
    metajson << "[";
    for (auto it = ranges.begin(); it != ranges.end(); it++) {
        auto r = it->second;
        metajson << "{";
        metajson << "\"flow\":" << r->flow << ",";
        metajson << "\"fhigh\":" << r->fhigh << ",";
        metajson << "\"fstep\":" << r->fstep << ",";
        metajson << "\"width\":" << r->width << ",";
        metajson << "\"specs\":[";
        for (auto imgmeta = r->imgmetas.begin(); imgmeta != r->imgmetas.end(); imgmeta++) {
            metajson << "{";
            metajson << "\"ts_start\":" << imgmeta->ts_start << ",";
            metajson << "\"height\":" << imgmeta->height << ",";
            metajson << "\"fname\":\"" << imgmeta->fname << "\"";
            metajson << "}";
            if (std::next(imgmeta) != r->imgmetas.end()) {
                metajson << ",";
            }
        }
        metajson << "]";
        metajson << "}";
        if (std::next(it) != ranges.end()) {
            metajson << ",";
        }
    }
    metajson << "]";
    std::ofstream outf("out/meta.json");
    if (!outf.is_open()) {
        EXIT_ERROR("could not open output metadata file");
    }
    outf << metajson.str();
    outf.close();

    return 0;
}
