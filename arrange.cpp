// arrange.cpp — generalized video-to-library matcher using OpenCV C++ API.
//
// Features:
//   - Configurable feature grid (N x N cells, G bits per cell, 1 or 3 channels)
//   - L1 distance matching (subsumes 1-bit Hamming when G=1)
//   - OpenMP-parallel matcher (match.c)
//   - Cross-frame tile cache for repeated patterns
//   - Hero pages: whitest/blackest by mean intensity (color-aware)
//
// Build (macOS):
//   clang -O3 -march=native -fopenmp -Xpreprocessor -fopenmp \
//       -I$(brew --prefix libomp)/include -L$(brew --prefix libomp)/lib -lomp \
//       $(pkg-config --cflags --libs opencv4) arrange.cpp match.c -o arrange
//
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include <opencv2/opencv.hpp>
#include <opencv2/core/utils/filesystem.hpp>

extern "C" void match_batch(const uint8_t* lib, const uint8_t* targets,
                            int n_pages, int num_targets, int feat_len, int* results);

// ---------------------------------------------------------------------------
// Global configuration (set from command line)
// ---------------------------------------------------------------------------
static int g_feat_n = 64;        // NxN grid
static int g_bits = 1;           // G bits per cell (1-8)
static bool g_color = false;     // 3 channels (BGR) if true, else 1 (gray)
static int g_channels = 1;       // computed from g_color
static int g_feat_len = 64;      // N*N*channels, computed at load time

static double g_max_block_pct = 0.5;   // max block is 50% of frame width
static double g_hero_min_pct = 0.0625; // hero blocks >= 6.25% of frame area
static double g_hero_min_w = 0, g_hero_min_h = 0; // computed per frame

static const int CELL = 8;  // coarse grid cell size (px)

// ---------------------------------------------------------------------------
// File IO helpers
// ---------------------------------------------------------------------------
static bool read_file(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0) { fclose(f); return false; }
    out.resize((size_t)sz);
    size_t rd = fread(out.data(), 1, (size_t)sz, f);
    fclose(f);
    return rd == (size_t)sz;
}

// ---------------------------------------------------------------------------
// Library loading (new features.bin format)
// ---------------------------------------------------------------------------
struct RegistryEntry {
    std::string pdf_path;
    int32_t page_idx;
};

static std::vector<uint8_t> g_features;
static std::vector<RegistryEntry> g_registry;
static int g_n_pages = 0;

static bool load_features(const std::string& path) {
    std::vector<uint8_t> buf;
    if (!read_file(path, buf)) {
        fprintf(stderr, "[ERROR] Cannot read features: %s\n", path.c_str());
        return false;
    }
    if (buf.size() < 16) { fprintf(stderr, "[ERROR] features.bin too small\n"); return false; }

    uint32_t n = 0, N = 0, G = 0, channels = 0;
    memcpy(&n, buf.data(), 4);
    memcpy(&N, buf.data() + 4, 4);
    memcpy(&G, buf.data() + 8, 4);
    memcpy(&channels, buf.data() + 12, 4);

    size_t need = 16 + (size_t)n * N * N * channels;
    if (buf.size() < need) { fprintf(stderr, "[ERROR] features.bin truncated\n"); return false; }

    g_n_pages = (int)n;
    g_feat_n = (int)N;
    g_bits = (int)G;
    g_color = (channels == 3);
    g_channels = channels;
    g_feat_len = N * N * channels;

    g_features.resize((size_t)n * g_feat_len);
    memcpy(g_features.data(), buf.data() + 16, (size_t)n * g_feat_len);
    return true;
}

static bool load_registry(const std::string& path) {
    std::vector<uint8_t> buf;
    if (!read_file(path, buf)) {
        fprintf(stderr, "[ERROR] Cannot read registry: %s\n", path.c_str());
        return false;
    }
    if (buf.size() < 4) { fprintf(stderr, "[ERROR] registry.bin too small\n"); return false; }
    uint32_t n = 0;
    memcpy(&n, buf.data(), 4);
    g_registry.resize((size_t)n);
    size_t off = 4;
    for (uint32_t i = 0; i < n; i++) {
        if (off + 8 > buf.size()) { fprintf(stderr, "[ERROR] registry.bin truncated\n"); return false; }
        int32_t page_idx = 0;
        uint32_t path_len = 0;
        memcpy(&page_idx, buf.data() + off, 4); off += 4;
        memcpy(&path_len, buf.data() + off, 4); off += 4;
        if (off + path_len > buf.size()) { fprintf(stderr, "[ERROR] registry.bin truncated\n"); return false; }
        g_registry[i].page_idx = page_idx;
        g_registry[i].pdf_path.assign((const char*)(buf.data() + off), (size_t)path_len);
        off += path_len;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Feature extraction: compute NxN grid, G bits per cell, 1 or 3 channels
// ---------------------------------------------------------------------------
static void compute_feature(const cv::Mat& crop, std::vector<uint8_t>& out) {
    int N = g_feat_n;
    int channels = g_channels;
    int G = g_bits;
    int max_val = (1 << G) - 1;

    cv::Mat resized;
    cv::resize(crop, resized, cv::Size(N, N), 0, 0, cv::INTER_AREA);

    std::vector<cv::Mat> channel_mats;
    if (g_color && resized.channels() == 3) {
        cv::split(resized, channel_mats);
    } else if (g_color && resized.channels() == 4) {
        cv::Mat tmp;
        cv::cvtColor(resized, tmp, cv::COLOR_BGRA2BGR);
        cv::split(tmp, channel_mats);
    } else if (!g_color) {
        if (resized.channels() > 1) {
            cv::cvtColor(resized, channel_mats.emplace_back(), cv::COLOR_BGR2GRAY);
        } else {
            channel_mats.push_back(resized);
        }
    }

    out.clear();
    out.reserve(N * N * channels);

    for (int ci = 0; ci < channels; ci++) {
        const cv::Mat& ch = channel_mats[ci];
        for (int r = 0; r < N; r++) {
            const uchar* row = ch.ptr<uchar>(r);
            for (int c = 0; c < N; c++) {
                int v = row[c];
                if (G < 8) v = (v * max_val) / 255;
                out.push_back((uint8_t)v);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Feature cache key (for cross-frame tile memo)
// ---------------------------------------------------------------------------
struct FeatureKey {
    std::vector<uint8_t> data;
    bool operator==(const FeatureKey& o) const { return data == o.data; }
};
namespace std {
    template<> struct hash<FeatureKey> {
        size_t operator()(const FeatureKey& k) const noexcept {
            size_t h = 1469598103934665603ULL;
            for (size_t i = 0; i < k.data.size(); i++) {
                h ^= k.data[i];
                h *= 1099511628211ULL;
            }
            return h;
        }
    };
}

static std::unordered_map<FeatureKey, int> g_tile_cache;

// ---------------------------------------------------------------------------
// Manifest record (24 bytes)
// ---------------------------------------------------------------------------
struct Instruction {
    int32_t x, y, w, h;
    int32_t op_id;     // library page index
    int32_t page_idx;  // registry[op_id].page_idx
};

// ---------------------------------------------------------------------------
// Greedy solver
// ---------------------------------------------------------------------------
struct Timings {
    double read = 0, gray = 0, solve = 0, match = 0, write = 0;
    long tiles = 0, cache_hits = 0;
};

static void solve_greedy(const cv::Mat& frame, int pid_white, int pid_black,
                         std::vector<Instruction>& manifest, Timings& t,
                         int max_block, int hero_min) {
    int h = frame.rows, w = frame.cols;

    cv::Mat gray;
    if (g_color) {
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    } else {
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    }

    cv::Mat sum_table;
    cv::integral(gray, sum_table, CV_32S);

    int gh = (h + 7) / 8, gw = (w + 7) / 8;
    std::vector<std::vector<bool>> visited8(gh, std::vector<bool>(gw, false));

    auto region_sum = [&](int cx, int cy, int cw, int ch) -> int {
        int x0 = cx * 8, y0 = cy * 8;
        int x1 = (cx + cw) * 8, y1 = (cy + ch) * 8;
        const int32_t* S = sum_table.ptr<int32_t>();
        int stride = sum_table.cols;
        return S[y1 * stride + x1] - S[y0 * stride + x1]
             - S[y1 * stride + x0] + S[y0 * stride + x0];
    };

    // Mean intensity in a region (0-255)
    auto region_mean = [&](int cx, int cy, int cw, int ch) -> double {
        int sum = region_sum(cx, cy, cw, ch);
        int area = cw * ch * 64;
        return area > 0 ? (double)(sum * 255) / area : 0.0;
    };

    // Is this region "pure enough" of the given color (0=black, 1=white)?
    auto is_pure = [&](int cx, int cy, int cw, int ch, int color) -> bool {
        double m = region_mean(cx, cy, cw, ch);
        double thresh = 127.0;
        if (color == 1) return m > thresh + 20.0;   // white-ish
        else return m < thresh - 20.0;              // black-ish
    };

    int max_cells = max_block / 8;
    int64_t t0 = cv::getTickCount();

    std::vector<Instruction> placeholders;
    std::vector<int> placeholder_inst_idx;
    std::vector<uint8_t> tiles_to_match;

    for (int cy = 0; cy < gh; cy++) {
        int y = cy * 8;
        for (int cx = 0; cx < gw; cx++) {
            int x = cx * 8;
            if (visited8[cy][cx]) continue;
            uint8_t color = (gray.at<uchar>(y, x) >= 127) ? 1 : 0;
            int mcw = 1, mch = 1;

            // Horizontal grow
            while (cx + mcw + 1 <= gw && mcw + 1 <= max_cells) {
                bool any_visited = false;
                for (int yy = cy; yy < cy + mch; yy++)
                    if (visited8[yy][cx + mcw]) { any_visited = true; break; }
                if (any_visited) break;
                if (is_pure(cx, cy, mcw + 1, mch, color)) mcw++;
                else break;
            }
            // Vertical grow
            while (cy + mch + 1 <= gh && mch + 1 <= max_cells) {
                bool any_visited = false;
                for (int xx = cx; xx < cx + mcw; xx++)
                    if (visited8[cy + mch][xx]) { any_visited = true; break; }
                if (any_visited) break;
                if (is_pure(cx, cy, mcw, mch + 1, color)) mch++;
                else break;
            }

            int mw = mcw * 8, mh = mch * 8;
            if (x + mw > w) mw = w - x;
            if (y + mh > h) mh = h - y;
            for (int yy = cy; yy < cy + mch; yy++)
                for (int xx = cx; xx < cx + mcw; xx++)
                    visited8[yy][xx] = true;

            Instruction inst;
            inst.x = x; inst.y = y; inst.w = mw; inst.h = mh;

            if (mw >= hero_min && mh >= hero_min) {
                int pid = (color == 1) ? pid_white : pid_black;
                inst.op_id = pid;
                inst.page_idx = g_registry[pid].page_idx;
                manifest.push_back(inst);
            } else {
                cv::Mat crop = frame(cv::Rect(x, y, mw, mh));
                std::vector<uint8_t> feat;
                compute_feature(crop, feat);

                FeatureKey key{feat};
                auto it = g_tile_cache.find(key);
                if (it != g_tile_cache.end()) {
                    t.cache_hits++;
                    int pid = it->second;
                    inst.op_id = pid;
                    inst.page_idx = g_registry[pid].page_idx;
                    manifest.push_back(inst);
                } else {
                    int idx = (int)tiles_to_match.size() / g_feat_len;
                    tiles_to_match.insert(tiles_to_match.end(), feat.begin(), feat.end());
                    placeholder_inst_idx.push_back((int)manifest.size());
                    inst.op_id = -1;
                    inst.page_idx = -1;
                    manifest.push_back(inst);
                }
            }
        }
    }

    if (!tiles_to_match.empty()) {
        int64_t tm = cv::getTickCount();
        int n = (int)(tiles_to_match.size() / g_feat_len);
        std::vector<int> results(n);
        match_batch(g_features.data(), tiles_to_match.data(), g_n_pages, n, g_feat_len, results.data());
        for (int i = 0; i < n; i++) {
            int pid = results[i];
            int inst_idx = placeholder_inst_idx[i];
            manifest[inst_idx].op_id = pid;
            manifest[inst_idx].page_idx = g_registry[pid].page_idx;
            FeatureKey key{tiles_to_match.data() + (size_t)i * g_feat_len,
                           (size_t)g_feat_len};
            g_tile_cache[key] = pid;
        }
        t.match += (cv::getTickCount() - tm) / cv::getTickFrequency();
        t.tiles += n;
    }

    t.solve += (cv::getTickCount() - t0) / cv::getTickFrequency();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char** argv) {
    int64_t start = cv::getTickCount();

    std::string feat_path = "features.bin";
    std::string reg_path = "registry.bin";
    std::string video_path = "../badapple.mp4";
    std::string manifest_dir = "manifests_greedy";
    int max_frames = 0;
    double scale = 15.0;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--max-frames" && i + 1 < argc) { max_frames = atoi(argv[++i]); }
        else if (a == "--video" && i + 1 < argc) { video_path = argv[++i]; }
        else if (a == "--manifests" && i + 1 < argc) { manifest_dir = argv[++i]; }
        else if (a == "--features" && i + 1 < argc) { feat_path = argv[++i]; }
        else if (a == "--registry" && i + 1 < argc) { reg_path = argv[++i]; }
        else if (a == "--scale" && i + 1 < argc) { scale = atof(argv[++i]); }
        else if (a == "--feat" && i + 1 < argc) { g_feat_n = atoi(argv[++i]); }
        else if (a == "--bits" && i + 1 < argc) { g_bits = atoi(argv[++i]); }
        else if (a == "--color") { g_color = true; }
        else if (a == "--max-block-pct" && i + 1 < argc) { g_max_block_pct = atof(argv[++i]); }
        else if (a == "--hero-min-pct" && i + 1 < argc) { g_hero_min_pct = atof(argv[++i]); }
    }

    if (!load_features(feat_path)) return 1;
    if (!load_registry(reg_path)) return 1;
    fprintf(stderr, "[PERF] Library loaded: %d pages, N=%d, G=%d, channels=%d\n", g_n_pages, g_feat_n, g_bits, g_channels);

    // Hero pages: argmax/argmin mean intensity across library features
    int pid_white = 0, pid_black = 0;
    double mean_max = -1, mean_min = 256;
    for (int i = 0; i < g_n_pages; i++) {
        double sum = 0;
        const uint8_t* f = &g_features[(size_t)i * g_feat_len];
        for (int j = 0; j < g_feat_len; j++) sum += f[j];
        double mean = sum / g_feat_len;
        if (mean > mean_max) { mean_max = mean; pid_white = i; }
        if (mean < mean_min) { mean_min = mean; pid_black = i; }
    }

    if (!cv::utils::fs::exists(video_path)) {
        video_path = "badapple.mp4";
        if (!cv::utils::fs::exists(video_path)) {
            fprintf(stderr, "[ERROR] Cannot find video: %s\n", video_path.c_str());
            return 1;
        }
    }

    cv::VideoCapture cap(video_path);
    if (!cap.isOpened()) {
        fprintf(stderr, "[ERROR] Cannot open video: %s\n", video_path.c_str());
        return 1;
    }

    // Get actual frame dimensions for block sizing
    int frame_w = (int)cap.get(cv::CAP_PROP_FRAME_WIDTH);
    int frame_h = (int)cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    g_hero_min_w = frame_w * g_hero_min_pct;
    g_hero_min_h = frame_h * g_hero_min_pct;
    int max_block = (int)(frame_w * g_max_block_pct);
    if (max_block % CELL != 0) max_block = (max_block / CELL) * CELL;
    if (max_block < CELL) max_block = CELL;

    int total = (int)cap.get(cv::CAP_PROP_FRAME_COUNT);
    if (max_frames > 0) total = (max_frames < total) ? max_frames : total;

    if (!cv::utils::fs::exists(manifest_dir))
        cv::utils::fs::createDirectory(manifest_dir);

    Timings t;
    int frames_done = 0;
    int total_tiles = 0;

    for (int i = 0; i < total; i++) {
        int64_t tr = cv::getTickCount();
        cv::Mat frame;
        if (!cap.read(frame)) break;
        t.read += (cv::getTickCount() - tr) / cv::getTickFrequency();

        int64_t tg = cv::getTickCount();
        cv::Mat gray;
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
        t.gray += (cv::getTickCount() - tg) / cv::getTickFrequency();

        std::vector<Instruction> manifest;
        solve_greedy(gray, pid_white, pid_black, manifest, t, max_block, max(g_hero_min_w, g_hero_min_h));

        for (const auto& e : manifest)
            if (e.op_id >= 0) total_tiles++;

        int64_t tw = cv::getTickCount();
        char fname[512];
        snprintf(fname, sizeof(fname), "%s/%04d.bin", manifest_dir.c_str(), i);
        FILE* f = fopen(fname, "wb");
        if (!f) {
            fprintf(stderr, "[ERROR] Cannot write %s\n", fname);
            return 1;
        }
        uint32_t n = (uint32_t)manifest.size();
        fwrite(&n, 4, 1, f);
        for (const auto& e : manifest) {
            int32_t buf[6] = { e.x, e.y, e.w, e.h, e.op_id, e.page_idx };
            fwrite(buf, 4, 6, f);
        }
        fclose(f);
        t.write += (cv::getTickCount() - tw) / cv::getTickFrequency();

        frames_done++;
        if (frames_done % 100 == 0) {
            double elapsed = (cv::getTickCount() - start) / cv::getTickFrequency();
            double fps = frames_done / (elapsed > 0 ? elapsed : 1);
            long detail = t.tiles + t.cache_hits;
            double hit = detail ? 100.0 * t.cache_hits / detail : 0.0;
            printf("frame %d/%d | %.2f fps | cache %.1f%% hit\n", frames_done, total, fps, hit);
            fflush(stdout);
        }
    }
    cap.release();

    double total_time = (cv::getTickCount() - start) / cv::getTickFrequency();
    double fps = frames_done ? frames_done / (total_time > 0 ? total_time : 1) : 0;
    long detail = t.tiles + t.cache_hits;
    double hit_rate = detail ? 100.0 * t.cache_hits / detail : 0.0;

    printf("[PERF] arrange complete in %.2fs | %.2f fps | %d tiles across %d frames\n",
           total_time, fps, total_tiles, frames_done);
    printf("[PERF] tile cache: %ld hits / %ld detail tiles = %.1f%% cache hit rate\n",
           t.cache_hits, detail, hit_rate);
    printf("[PERF] phase breakdown (total seconds across all frames):\n");
    auto phase = [&](const char* name, double v) {
        double pct = total_time ? 100.0 * v / total_time : 0.0;
        printf("         %-6s: %7.2fs  (%4.1f%%)\n", name, v, pct);
    };
    phase("read", t.read); phase("gray", t.gray); phase("solve", t.solve);
    phase("match", t.match); phase("write", t.write);
    printf("         detail tiles sent to C matcher: %ld\n", t.tiles);
    fflush(stdout);
    return 0;
}