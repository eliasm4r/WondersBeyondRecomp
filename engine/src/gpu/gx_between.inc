constexpr float kMaxStep = 0.5f;

bool close_enough(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    if (std::isnan(a[0]) || std::isnan(b[0]) || a[3] <= 0.0f || b[3] <= 0.0f) {
        return true;
    }
    return std::fabs(b[0] / b[3] - a[0] / a[3]) <= kMaxStep && std::fabs(b[1] / b[3] - a[1] / a[3]) <= kMaxStep;
}

bool close_draw(const RecordedDraw& before, const RecordedDraw& draw) {
    uint32_t step = std::max(1u, draw.positions / 8);
    for (uint32_t i = 0; i < draw.positions; i += step) {
        if (!close_enough(g_previous.positions[before.first_position + i], g_record.positions[draw.first_position + i])) {
            return false;
        }
    }
    return true;
}

constexpr uint32_t kUnmatched = ~0u;
constexpr uint32_t kRejected = ~0u - 1;
constexpr uint32_t kByCamera = 0xFFFFFF00u;

constexpr double kStillScreen = 2e-3;

struct CameraMotion {
    uint32_t id = 0;
    bool claimed = false;
    bool known = false;
    float back[4][4] = {};
    size_t second = kMaxCameras;
    bool is_second = false;
    float reach = 0.0f;
    std::vector<std::array<float, 4>> from;
    std::vector<std::array<float, 4>> to;
};

CameraMotion g_cameras[kMaxCameras];

double g_time_pairing = 0.0;
double g_time_filling = 0.0;
double g_time_sending = 0.0;

std::vector<uint8_t> g_world_camera;
std::vector<uint8_t> g_world_moving;
std::vector<std::array<float, 2>> g_decal_shift;
double g_forward[kMaxCameras][4][4];

bool invert(const float (&in)[4][4], double (&out)[4][4]) {
    double m[4][8];
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            m[r][c] = in[r][c];
            m[r][4 + c] = r == c ? 1.0 : 0.0;
        }
    }
    for (int column = 0; column < 4; column++) {
        int pivot = column;
        for (int r = column + 1; r < 4; r++) {
            if (std::fabs(m[r][column]) > std::fabs(m[pivot][column])) {
                pivot = r;
            }
        }
        if (std::fabs(m[pivot][column]) < 1e-300) {
            return false;
        }
        std::swap(m[column], m[pivot]);
        double scale = 1.0 / m[column][column];
        for (double& value : m[column]) {
            value *= scale;
        }
        for (int r = 0; r < 4; r++) {
            if (r != column && m[r][column] != 0.0) {
                double factor = m[r][column];
                for (int c = 0; c < 8; c++) {
                    m[r][c] -= factor * m[column][c];
                }
            }
        }
    }
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            out[r][c] = m[r][4 + c];
        }
    }
    return true;
}

enum Stillness : uint8_t { kUntested, kStill, kMoved };

Stillness stillness(const std::array<float, 4>& a, const std::array<float, 4>& b, const CameraMotion& camera) {
    constexpr double kScreen = kStillScreen;
    constexpr double kDepth = 2e-5;
    if (std::isnan(a[0]) || std::isnan(b[0]) || a[3] <= 0.0f || b[3] <= 0.0f || std::fabs(b[0]) > 1.5f * b[3] || std::fabs(b[1]) > 1.5f * b[3]) {
        return kUntested;
    }
    double p[4];
    for (int r = 0; r < 4; r++) {
        p[r] = static_cast<double>(camera.back[r][0]) * b[0] + static_cast<double>(camera.back[r][1]) * b[1] + static_cast<double>(camera.back[r][2]) * b[2] +
               static_cast<double>(camera.back[r][3]) * b[3];
    }
    bool moved = p[3] <= 0.0 || std::fabs(p[0] / p[3] - a[0] / a[3]) > kScreen || std::fabs(p[1] / p[3] - a[1] / a[3]) > kScreen ||
                 std::fabs(p[2] / p[3] - a[2] / a[3]) > kDepth;
    return moved ? kMoved : kStill;
}

bool moves_in_world(const RecordedDraw& draw, const RecordedDraw& before, const CameraMotion& camera) {
    for (uint32_t i = 0; i < draw.positions; i++) {
        if (stillness(g_previous.positions[before.first_position + i], g_record.positions[draw.first_position + i], camera) == kMoved) {
            return true;
        }
    }
    return false;
}

using Row = std::array<float, 4>;

struct RunCamera {
    bool valid = false;
    float move[3][4] = {};
    bool known = false;
    uint32_t id = 0;
    float back[4][4] = {};
};

RunCamera g_run_camera;
bool g_run_cut = false;
std::array<std::string, 256> g_pair_notes;
bool g_pair_notes_wanted = false;
uint32_t g_run_counts[6] = {};
std::vector<Row> g_replay_rows;
std::vector<RunDraw> g_replay_draws;

enum MatrixChange : uint8_t { kSameMatrix, kNearMatrix, kOtherMatrix };

MatrixChange matrix_change(const Row* a, const Row* b) {
    float scale = 0.0f;
    float turn = 0.0f;
    float step = 0.0f;
    float length = 0.0f;
    bool same = true;
    bool turned = false;
    for (int i = 0; i < 3; i++) {
        float row_scale = 0.0f;
        float row_turn = 0.0f;
        for (int j = 0; j < 4; j++) {
            float x = a[i][j];
            float y = b[i][j];
            if (!std::isfinite(x) || !std::isfinite(y)) {
                return kOtherMatrix;
            }
            same = same && x == y;
            if (j == 3) {
                step += (y - x) * (y - x);
                length += y * y;
            } else {
                scale = std::max(scale, std::fabs(y));
                turn = std::max(turn, std::fabs(y - x));
                row_scale = std::max({row_scale, std::fabs(x), std::fabs(y)});
                row_turn = std::max(row_turn, std::fabs(y - x));
            }
        }
        turned = turned || row_turn > 0.5f * row_scale;
    }
    if (same) {
        return kSameMatrix;
    }
    return turned || turn > 0.3f * scale || std::sqrt(step) > 0.5f * std::sqrt(length) + 200.0f ? kOtherMatrix : kNearMatrix;
}

void lerp_rows(Row* out, const Row* a, const Row* b, uint32_t count, float t) {
    for (uint32_t i = 0; i < count; i++) {
        for (int j = 0; j < 4; j++) {
            float x = a[i][j];
            float y = b[i][j];
            if (x != y && std::isfinite(x) && std::isfinite(y)) {
                out[i][j] = x + (y - x) * t;
            }
        }
    }
}

void blend_matrix(Row* out, const Row* a, const Row* b, float t) {
    MatrixChange change = matrix_change(a, b);
    if (change == kNearMatrix) {
        lerp_rows(out, a, b, 3, t);
    } else if (change == kOtherMatrix && t < 0.5f) {
        std::copy(a, a + 3, out);
    }
}

constexpr float kFirstTexCoordSource = 5.0f;
constexpr float kLastTexCoordSource = 12.0f;
constexpr float kTextureJump = 0.05f;

bool texture_jumped(const Row* a, const Row* b) {
    for (int i = 0; i < 2; i++) {
        if (std::fabs(b[i][3] - a[i][3]) > kTextureJump) {
            return true;
        }
    }
    return false;
}

constexpr float kTextureSlide = 1e-4f;

bool texture_slid(const Row* a, const Row* b) {
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            if (a[i][j] != b[i][j]) {
                return false;
            }
        }
    }
    return std::fabs(b[0][3] - a[0][3]) > kTextureSlide || std::fabs(b[1][3] - a[1][3]) > kTextureSlide;
}

struct TextureScroll {
    uint64_t frame = 0;
    float now[2] = {};
    float before[2] = {};
};

std::unordered_map<uint64_t, TextureScroll> g_scrolls;

float unwrapped(float step) {
    return step - std::round(step);
}

bool scrolls_steadily(uint64_t key, const Row* a, const Row* b) {
    if (g_scrolls.size() > 8192) {
        g_scrolls.clear();
    }
    TextureScroll& scroll = g_scrolls[key];
    if (scroll.frame != g_frame) {
        for (int i = 0; i < 2; i++) {
            scroll.before[i] = scroll.frame + 1 == g_frame ? scroll.now[i] : NAN;
            scroll.now[i] = unwrapped(b[i][3] - a[i][3]);
        }
        scroll.frame = g_frame;
    }
    for (int i = 0; i < 2; i++) {
        if (!(std::fabs(scroll.now[i] - scroll.before[i]) < 2e-3f) || std::fabs(scroll.now[i]) >= 0.45f) {
            return false;
        }
    }
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            if (a[i][j] != b[i][j]) {
                return false;
            }
        }
    }
    return true;
}

void blend_texture_matrix(Row* out, const Row* a, const Row* b, float t, bool from_coordinates, uint64_t key) {
    bool steady = from_coordinates && scrolls_steadily(key, a, b);
    if (steady && texture_jumped(a, b)) {
        for (int i = 0; i < 2; i++) {
            out[i][3] = b[i][3] - unwrapped(b[i][3] - a[i][3]) * (1.0f - t);
        }
        return;
    }
    MatrixChange change = from_coordinates && (texture_jumped(a, b) || (!steady && texture_slid(a, b))) ? kOtherMatrix : matrix_change(a, b);
    if (change == kNearMatrix) {
        lerp_rows(out, a, b, 3, t);
    } else if (change == kOtherMatrix && t < 0.5f) {
        std::copy(a, a + 3, out);
    }
    g_run_counts[3] += change == kOtherMatrix;
}

bool same_shape(const RunDraw& before, const RunDraw& now, const Row* a, const Row* b) {
    if (before.rows != now.rows || a[1][2] != b[1][2] || a[1][3] != b[1][3] || a[3][3] != b[3][3] || (before.palette == kNoPalette) != (now.palette == kNoPalette)) {
        return false;
    }
    uint32_t setup = static_cast<uint32_t>(b[3][3]);
    uint32_t units = std::min(setup & 15u, 8u);
    bool lit = (setup & 32u) != 0;
    uint32_t light = kRunHeaderRows + kRunUnitRows * units;
    if (light + (lit ? kRunLightRows : 0) > now.rows) {
        return false;
    }
    for (uint32_t i = 0; i < units; i++) {
        if (a[kRunHeaderRows + kRunUnitRows * i] != b[kRunHeaderRows + kRunUnitRows * i]) {
            return false;
        }
    }
    return !lit || a[light] == b[light];
}

void blend_run(Row* out, const Row* a, const Row* b, float t, uint64_t key) {
    lerp_rows(out, a, b, 2, t);
    bool own = b[1][3] != 0.0f;
    if (own) {
        blend_matrix(out + 4, a + 4, b + 4, t);
    }
    uint32_t setup = static_cast<uint32_t>(b[3][3]);
    uint32_t units = std::min(setup & 15u, 8u);
    for (uint32_t i = 0; i < units; i++) {
        uint32_t at = kRunHeaderRows + kRunUnitRows * i;
        bool from_coordinates = b[at][0] >= kFirstTexCoordSource && b[at][0] <= kLastTexCoordSource;
        if (b[at][2] != 0.0f) {
            blend_texture_matrix(out + at + 1, a + at + 1, b + at + 1, t, from_coordinates, key * 31 + i * 2);
        }
        blend_texture_matrix(out + at + 4, a + at + 4, b + at + 4, t, from_coordinates, key * 31 + i * 2 + 1);
    }
    if (setup & 32u) {
        uint32_t light = kRunHeaderRows + kRunUnitRows * units;
        if (own) {
            lerp_rows(out + light + 5, a + light + 5, b + light + 5, 3, t);
        }
        for (uint32_t n = 0; n < 8; n++) {
            lerp_rows(out + light + 9 + n * 4, a + light + 9 + n * 4, b + light + 9 + n * 4, 3, t);
        }
    }
}

bool split_matrix(const Row* m, float (&q)[4], float (&scale)[3]) {
    float r[3][3];
    for (int j = 0; j < 3; j++) {
        scale[j] = std::sqrt(m[0][j] * m[0][j] + m[1][j] * m[1][j] + m[2][j] * m[2][j]);
        if (!std::isfinite(scale[j]) || scale[j] < 1e-6f) {
            return false;
        }
        for (int i = 0; i < 3; i++) {
            r[i][j] = m[i][j] / scale[j];
        }
    }
    float det = r[0][0] * (r[1][1] * r[2][2] - r[1][2] * r[2][1]) - r[0][1] * (r[1][0] * r[2][2] - r[1][2] * r[2][0]) + r[0][2] * (r[1][0] * r[2][1] - r[1][1] * r[2][0]);
    if (det < 0.9f || det > 1.1f) {
        return false;
    }
    float w = std::sqrt(std::max(0.0f, 1.0f + r[0][0] + r[1][1] + r[2][2])) / 2;
    float x = std::copysign(std::sqrt(std::max(0.0f, 1.0f + r[0][0] - r[1][1] - r[2][2])) / 2, r[2][1] - r[1][2]);
    float y = std::copysign(std::sqrt(std::max(0.0f, 1.0f - r[0][0] + r[1][1] - r[2][2])) / 2, r[0][2] - r[2][0]);
    float z = std::copysign(std::sqrt(std::max(0.0f, 1.0f - r[0][0] - r[1][1] + r[2][2])) / 2, r[1][0] - r[0][1]);
    float length = std::sqrt(w * w + x * x + y * y + z * z);
    q[0] = w / length;
    q[1] = x / length;
    q[2] = y / length;
    q[3] = z / length;
    return true;
}

constexpr float kLargestBoneTurn = 1.6f;

bool blend_turn(Row* out, const Row* a, const Row* b, float t) {
    float qa[4], qb[4], sa[3], sb[3];
    if (!split_matrix(a, qa, sa) || !split_matrix(b, qb, sb)) {
        return false;
    }
    float step = 0.0f;
    float length = 0.0f;
    for (int i = 0; i < 3; i++) {
        if (std::fabs(sa[i] - sb[i]) > 0.1f * std::max(sa[i], sb[i])) {
            return false;
        }
        step += (b[i][3] - a[i][3]) * (b[i][3] - a[i][3]);
        length += b[i][3] * b[i][3];
    }
    if (std::sqrt(step) > 0.5f * std::sqrt(length) + 200.0f) {
        return false;
    }
    float dot = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
    float sign = dot < 0.0f ? -1.0f : 1.0f;
    float angle = std::acos(std::min(1.0f, std::fabs(dot)));
    if (2 * angle > kLargestBoneTurn) {
        return false;
    }
    float wa = 1.0f - t;
    float wb = t;
    if (angle > 1e-4f) {
        wa = std::sin((1.0f - t) * angle) / std::sin(angle);
        wb = std::sin(t * angle) / std::sin(angle);
    }
    float q[4];
    for (int i = 0; i < 4; i++) {
        q[i] = wa * qa[i] + wb * sign * qb[i];
    }
    float w = q[0], x = q[1], y = q[2], z = q[3];
    float r[3][3] = {{1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)},
                     {2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)},
                     {2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)}};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            out[i][j] = r[i][j] * (sa[j] + (sb[j] - sa[j]) * t);
        }
        out[i][3] = a[i][3] + (b[i][3] - a[i][3]) * t;
    }
    return true;
}

void blend_palette(Row* out, const Row* a, const Row* b, float t) {
    for (uint32_t m = 0; m + 3 <= 64; m += 3) {
        if (matrix_change(a + m, b + m) != kOtherMatrix || !blend_turn(out + m, a + m, b + m, t)) {
            blend_matrix(out + m, a + m, b + m, t);
        }
    }
    lerp_rows(out + kPaletteRows, a + kPaletteRows, b + kPaletteRows, kNormalRows, t);
}

bool run_perspective(const FrameRecord& record, const RecordedRun& run) {
    return record.rows[run.draw.row + 1][2] == 0.0f;
}

bool load_matrix(const Row* rows, float (&out)[3][4]) {
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 4; j++) {
            float value = rows[i][j];
            if (!std::isfinite(value) || std::fabs(value) > 1e7f) {
                return false;
            }
            out[i][j] = value;
        }
    }
    return true;
}

bool invert_affine(const float (&m)[3][4], float (&out)[3][4]) {
    float c[3][3];
    c[0][0] = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    c[0][1] = m[0][2] * m[2][1] - m[0][1] * m[2][2];
    c[0][2] = m[0][1] * m[1][2] - m[0][2] * m[1][1];
    c[1][0] = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    c[1][1] = m[0][0] * m[2][2] - m[0][2] * m[2][0];
    c[1][2] = m[0][2] * m[1][0] - m[0][0] * m[1][2];
    c[2][0] = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    c[2][1] = m[0][1] * m[2][0] - m[0][0] * m[2][1];
    c[2][2] = m[0][0] * m[1][1] - m[0][1] * m[1][0];
    float det = m[0][0] * c[0][0] + m[0][1] * c[1][0] + m[0][2] * c[2][0];
    if (std::fabs(det) < 1e-12f) {
        return false;
    }
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            out[i][j] = c[i][j] / det;
        }
        out[i][3] = -(out[i][0] * m[0][3] + out[i][1] * m[1][3] + out[i][2] * m[2][3]);
    }
    return true;
}

bool close_motion(const float (&a)[3][4], const float (&b)[3][4]) {
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            if (std::fabs(a[i][j] - b[i][j]) > 0.02f) {
                return false;
            }
        }
        if (std::fabs(a[i][3] - b[i][3]) > 2.0f + 0.02f * std::fabs(b[i][3])) {
            return false;
        }
    }
    return true;
}

bool clip_matrix(const FrameRecord& record, const RecordedRun& run, float (&out)[4][4]) {
    const Row* h = &record.rows[run.draw.row];
    const Row* m = &record.rows[run.draw.matrix];
    double x[4] = {h[0][0], 0.0, 0.0, 0.0};
    double y[4] = {0.0, h[0][2], 0.0, 0.0};
    double z[4] = {0.0, 0.0, h[1][0], h[1][1]};
    double w[4] = {0.0, 0.0, 0.0, 0.0};
    if (h[1][2] != 0.0f) {
        x[3] = h[0][1];
        y[3] = h[0][3];
        w[3] = 1.0;
    } else {
        x[2] = h[0][1];
        y[2] = h[0][3];
        w[2] = -1.0;
    }
    double clip[4][4];
    for (int j = 0; j < 4; j++) {
        clip[0][j] = x[j] * h[2][0] + w[j] * h[2][1];
        clip[1][j] = y[j] * h[2][2] + w[j] * h[2][3];
        clip[2][j] = (w[j] * h[3][1] - z[j] * h[3][0]) / 16777215.0;
        clip[3][j] = w[j];
    }
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            double sum = c == 3 ? clip[r][3] : 0.0;
            for (int k = 0; k < 3; k++) {
                sum += clip[r][k] * m[k][c];
            }
            if (!std::isfinite(sum)) {
                return false;
            }
            out[r][c] = static_cast<float>(sum);
        }
    }
    return true;
}

void find_run_camera(const std::vector<uint32_t>& unique) {
    constexpr size_t kSamples = 48;
    static float moves[kSamples][3][4];
    static uint32_t sampled[kSamples];
    size_t total = 0;
    for (uint32_t before : unique) {
        total += before != kUnmatched;
    }
    size_t step = total / kSamples + 1;
    size_t count = 0;
    size_t seen = 0;
    for (size_t i = 0; i < unique.size() && count < kSamples; i++) {
        if (unique[i] == kUnmatched || seen++ % step != 0) {
            continue;
        }
        const RecordedRun& run = g_record.runs[i];
        const RecordedRun& before = g_previous.runs[unique[i]];
        float now[3][4];
        float then[3][4];
        float back[3][4];
        if (!run_perspective(g_record, run) || !run_perspective(g_previous, before) || !load_matrix(&g_record.rows[run.draw.matrix], now) ||
            !load_matrix(&g_previous.rows[before.draw.matrix], then) || !invert_affine(then, back)) {
            continue;
        }
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 4; c++) {
                moves[count][r][c] = now[r][0] * back[0][c] + now[r][1] * back[1][c] + now[r][2] * back[2][c] + (c == 3 ? now[r][3] : 0.0f);
            }
        }
        sampled[count++] = static_cast<uint32_t>(i);
    }
    size_t best = 0;
    size_t best_votes = 0;
    for (size_t i = 0; i < count; i++) {
        size_t votes = 0;
        for (size_t k = 0; k < count; k++) {
            votes += close_motion(moves[i], moves[k]);
        }
        if (votes > best_votes) {
            best_votes = votes;
            best = i;
        }
    }
    g_run_camera.valid = best_votes >= 3 && best_votes * 3 >= count;
    g_run_camera.known = false;
    if (!g_run_camera.valid) {
        return;
    }
    std::memcpy(g_run_camera.move, moves[best], sizeof g_run_camera.move);
    const RecordedRun& run = g_record.runs[sampled[best]];
    const RecordedRun& before = g_previous.runs[unique[sampled[best]]];
    float now[4][4];
    float then[4][4];
    double inverse[4][4];
    if (run.draw.camera == before.draw.camera && clip_matrix(g_record, run, now) && clip_matrix(g_previous, before, then) && invert(now, inverse)) {
        g_run_camera.known = true;
        g_run_camera.id = run.draw.camera;
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                double sum = 0.0;
                for (int k = 0; k < 4; k++) {
                    sum += then[r][k] * inverse[k][c];
                }
                g_run_camera.back[r][c] = static_cast<float>(sum);
                g_run_camera.known = g_run_camera.known && std::isfinite(sum);
            }
        }
    }
}

constexpr float kFarthestStep = 0.1f;
constexpr float kTeleportStep = 0.75f;
constexpr uint32_t kMostCopies = 4;

float run_distance(const RecordedRun& before, const RecordedRun& run, bool with_camera = true) {
    const Row* a = &g_previous.rows[before.draw.matrix];
    const Row* b = &g_record.rows[run.draw.matrix];
    float distance = 0.0f;
    if (with_camera && g_run_camera.valid && run_perspective(g_previous, before) && run_perspective(g_record, run)) {
        for (int i = 0; i < 3; i++) {
            const float* r = g_run_camera.move[i];
            distance += std::fabs(r[0] * a[0][3] + r[1] * a[1][3] + r[2] * a[2][3] + r[3] - b[i][3]);
        }
    } else {
        for (int i = 0; i < 3; i++) {
            distance += std::fabs(b[i][3] - a[i][3]);
        }
    }
    return std::isfinite(distance) ? distance : 1e30f;
}

bool same_object(const RecordedRun& before, const RecordedRun& run, float distance, float step) {
    if (!run_perspective(g_previous, before) || !run_perspective(g_record, run)) {
        return true;
    }
    const Row* b = &g_record.rows[run.draw.matrix];
    float away = std::sqrt(b[0][3] * b[0][3] + b[1][3] * b[1][3] + b[2][3] * b[2][3]);
    return distance <= step * away;
}

float copy_distance(const RecordedRun& before, const RecordedRun& run) {
    return std::min(run_distance(before, run), run_distance(before, run, false));
}

void pair_runs() {
    auto started = std::chrono::steady_clock::now();
    std::vector<RecordedRun>& runs = g_record.runs;
    const std::vector<RecordedRun>& before = g_previous.runs;
    g_run_cut = false;
    g_run_camera.valid = false;
    g_run_camera.known = false;
    for (RecordedRun& run : runs) {
        run.match = kUnmatched;
    }
    if (runs.empty() || before.empty()) {
        return;
    }
    constexpr uint32_t kCandidates = 48;
    static std::vector<uint64_t> keys;
    static std::vector<uint32_t> head;
    static std::vector<uint32_t> next;
    static std::vector<uint32_t> unique;
    size_t size = 64;
    while (size < before.size() * 2) {
        size *= 2;
    }
    keys.assign(size, 0);
    head.assign(size, kUnmatched);
    next.assign(before.size(), kUnmatched);
    auto slot_of = [size](uint64_t key) {
        size_t slot = static_cast<size_t>(key ^ (key >> 29)) & (size - 1);
        while (keys[slot] != 0 && keys[slot] != key) {
            slot = (slot + 1) & (size - 1);
        }
        return slot;
    };
    for (size_t i = before.size(); i-- > 0;) {
        uint64_t key = before[i].draw.key;
        if (key == 0) {
            continue;
        }
        size_t slot = slot_of(key);
        keys[slot] = key;
        next[i] = head[slot];
        head[slot] = static_cast<uint32_t>(i);
    }
    unique.assign(runs.size(), kUnmatched);
    static std::vector<uint32_t> last;
    static std::vector<uint32_t> later;
    last.assign(size, kUnmatched);
    later.assign(runs.size(), kUnmatched);
    for (size_t i = runs.size(); i-- > 0;) {
        uint64_t key = runs[i].draw.key;
        size_t slot = key != 0 ? slot_of(key) : 0;
        if (key != 0 && keys[slot] == key) {
            later[i] = last[slot];
            last[slot] = static_cast<uint32_t>(i);
        }
    }
    for (size_t i = 0; i < runs.size(); i++) {
        uint64_t key = runs[i].draw.key;
        size_t slot = key != 0 ? slot_of(key) : 0;
        if (key != 0 && keys[slot] == key && head[slot] != kUnmatched && next[head[slot]] == kUnmatched) {
            unique[i] = head[slot];
        }
    }
    find_run_camera(unique);
    uint32_t paired = 0;
    uint32_t rejected = 0;
    static std::vector<uint8_t> settled;
    settled.assign(runs.size(), 0);
    for (size_t i = 0; i < runs.size(); i++) {
        uint64_t key = runs[i].draw.key;
        size_t slot = key != 0 ? slot_of(key) : 0;
        if (key == 0 || keys[slot] != key || last[slot] != i || later[i] == kUnmatched) {
            continue;
        }
        std::array<uint32_t, kMostCopies> now_copies{};
        std::array<uint32_t, kMostCopies> then_copies{};
        uint32_t count = 0;
        uint32_t then_count = 0;
        for (uint32_t k = static_cast<uint32_t>(i); k != kUnmatched && count <= kMostCopies; k = later[k]) {
            if (count < kMostCopies) {
                now_copies[count] = k;
            }
            count++;
        }
        for (uint32_t k = head[slot]; k != kUnmatched && then_count <= kMostCopies; k = next[k]) {
            if (then_count < kMostCopies) {
                then_copies[then_count] = k;
            }
            then_count++;
        }
        if (count != then_count || count > kMostCopies) {
            continue;
        }
        std::array<uint32_t, kMostCopies> order{};
        std::array<uint32_t, kMostCopies> best_order{};
        for (uint32_t k = 0; k < count; k++) {
            order[k] = k;
        }
        float best_total = -1.0f;
        do {
            float total = 0.0f;
            for (uint32_t k = 0; k < count; k++) {
                total += run_distance(before[then_copies[order[k]]], runs[now_copies[k]]);
            }
            if (best_total < 0.0f || total < best_total) {
                best_total = total;
                best_order = order;
            }
        } while (std::next_permutation(order.begin(), order.begin() + count));
        for (uint32_t k = 0; k < count; k++) {
            RecordedRun& run = runs[now_copies[k]];
            uint32_t then = then_copies[best_order[k]];
            settled[now_copies[k]] = 1;
            if (!same_object(before[then], run, copy_distance(before[then], run), kFarthestStep)) {
                g_run_counts[4]++;
                continue;
            }
            run.match = then;
            paired++;
            rejected += matrix_change(&g_previous.rows[before[then].draw.matrix], &g_record.rows[run.draw.matrix]) == kOtherMatrix;
        }
        head[slot] = kUnmatched;
    }
    for (size_t i = 0; i < runs.size(); i++) {
        RecordedRun& run = runs[i];
        uint64_t key = run.draw.key;
        if (key == 0 || settled[i]) {
            continue;
        }
        size_t slot = slot_of(key);
        if (keys[slot] != key || head[slot] == kUnmatched) {
            continue;
        }
        uint32_t* link = &head[slot];
        uint32_t* best = link;
        float best_distance = next[*link] == kUnmatched && later[i] == kUnmatched ? 0.0f : run_distance(before[*link], run);
        link = &next[*link];
        for (uint32_t n = 1; n < kCandidates && *link != kUnmatched && best_distance > 0.0f; n++, link = &next[*link]) {
            float distance = run_distance(before[*link], run);
            if (distance < best_distance) {
                best_distance = distance;
                best = link;
            }
        }
        bool taken = false;
        for (uint32_t n = 0, other = later[i]; n < kCandidates && other != kUnmatched && !taken && best_distance > 0.0f; n++, other = later[other]) {
            taken = run_distance(before[*best], runs[other]) < best_distance;
        }
        if (taken) {
            continue;
        }
        bool alone = best_distance == 0.0f;
        if (!same_object(before[*best], run, alone ? run_distance(before[*best], run) : best_distance, alone ? kTeleportStep : kFarthestStep)) {
            g_run_counts[alone ? 5 : 4]++;
            continue;
        }
        run.match = *best;
        *best = next[*best];
        paired++;
        rejected += matrix_change(&g_previous.rows[before[run.match].draw.matrix], &g_record.rows[run.draw.matrix]) == kOtherMatrix;
    }
    static std::vector<uint8_t> used;
    static std::vector<uint32_t> left;
    used.assign(before.size(), 0);
    for (const RecordedRun& run : runs) {
        if (run.match != kUnmatched) {
            used[run.match] = 1;
        }
    }
    left.clear();
    for (uint32_t k = 0; k < before.size(); k++) {
        if (!used[k] && before[k].draw.shape != 0) {
            left.push_back(k);
        }
    }
    for (size_t i = 0; i < runs.size() && !left.empty(); i++) {
        RecordedRun& run = runs[i];
        if (run.match != kUnmatched || run.draw.shape == 0) {
            continue;
        }
        size_t best = left.size();
        float best_distance = 0.0f;
        for (size_t k = 0; k < left.size(); k++) {
            const RecordedRun& other = before[left[k]];
            if (other.draw.shape != run.draw.shape || other.draw.camera != run.draw.camera) {
                continue;
            }
            float distance = run_distance(other, run);
            if (best == left.size() || distance < best_distance) {
                best = k;
                best_distance = distance;
            }
        }
        if (best == left.size() || !same_object(before[left[best]], run, copy_distance(before[left[best]], run), kFarthestStep)) {
            continue;
        }
        run.match = left[best];
        left[best] = left.back();
        left.pop_back();
        paired++;
    }
    g_run_cut = rejected > 8 && rejected * 3 > paired;
    if (g_pair_notes_wanted) {
        std::string& note = g_pair_notes[g_frame % g_pair_notes.size()];
        char part[160];
        std::snprintf(part, sizeof part, "frame %llu runs %zu paired %u far %u cut %d:", static_cast<unsigned long long>(g_frame), runs.size(), paired, rejected, g_run_cut);
        note = part;
        for (size_t i = 0; i < runs.size() && note.size() < 6000; i++) {
            uint64_t key = runs[i].draw.key;
            size_t slot = key != 0 ? slot_of(key) : 0;
            uint32_t copies = 0;
            for (uint32_t other = key != 0 && keys[slot] == key ? last[slot] : kUnmatched; other != kUnmatched && copies < 4; other = later[other]) {
                copies++;
            }
            if (copies < 4) {
                continue;
            }
            const Row* b = &g_record.rows[runs[i].draw.matrix];
            float size = std::max({std::fabs(b[0][0]), std::fabs(b[0][1]), std::fabs(b[0][2])});
            if (runs[i].match == kUnmatched) {
                std::snprintf(part, sizeof part, " | %zu new %04x at %.1f %.1f %.1f size %.2f", i, static_cast<unsigned>(key & 0xFFFF), b[0][3], b[1][3], b[2][3], size);
            } else {
                const Row* a = &g_previous.rows[before[runs[i].match].draw.matrix];
                std::snprintf(part, sizeof part, " | %zu<-%u %04x kind %d from %.1f %.1f %.1f to %.1f %.1f %.1f size %.2f<-%.2f", i, runs[i].match, static_cast<unsigned>(key & 0xFFFF),
                              static_cast<int>(matrix_change(a, b)), a[0][3], a[1][3], a[2][3], b[0][3], b[1][3], b[2][3], size,
                              std::max({std::fabs(a[0][0]), std::fabs(a[0][1]), std::fabs(a[0][2])}));
            }
            note += part;
        }
    }
    g_run_counts[0] += static_cast<uint32_t>(runs.size());
    g_run_counts[1] += paired;
    g_run_counts[2] += rejected;
    g_time_pairing += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

void adopt_run_camera() {
    if (!g_run_camera.known) {
        return;
    }
    CameraMotion* chosen = nullptr;
    for (CameraMotion& camera : g_cameras) {
        if (camera.claimed && camera.id == g_run_camera.id && !camera.is_second) {
            chosen = camera.known ? nullptr : &camera;
            break;
        }
        if (!camera.claimed && !chosen) {
            chosen = &camera;
        }
    }
    if (chosen) {
        chosen->claimed = true;
        chosen->known = true;
        chosen->id = g_run_camera.id;
        std::memcpy(chosen->back, g_run_camera.back, sizeof chosen->back);
    }
}

bool replay_runs(const RecordedCommand& command, float t) {
    uint32_t palette_at = kNoPalette;
    uint32_t palette_now = kNoPalette;
    uint32_t palette_before = kNoPalette;
    for (size_t i = command.begin; i < command.end; i++) {
        const RecordedRun& run = g_record.runs[i];
        const Row* now = &g_record.rows[run.draw.row];
        const RecordedRun* before = run.match != kUnmatched ? &g_previous.runs[run.match] : nullptr;
        const Row* then = before ? &g_previous.rows[before->draw.row] : nullptr;
        if (before && !same_shape(before->draw, run.draw, then, now)) {
            before = nullptr;
        }
        if (g_replay_rows.size() + run.draw.rows + kPaletteRows + kNormalRows > kRunRowBatch) {
            if (!send_replayed()) {
                return false;
            }
            palette_at = kNoPalette;
        }
        if (run.draw.palette != kNoPalette) {
            uint32_t other = before ? before->draw.palette : kNoPalette;
            if (palette_at == kNoPalette || palette_now != run.draw.palette || palette_before != other) {
                palette_at = static_cast<uint32_t>(g_replay_rows.size());
                palette_now = run.draw.palette;
                palette_before = other;
                const Row* palette = &g_record.rows[run.draw.palette];
                g_replay_rows.insert(g_replay_rows.end(), palette, palette + kPaletteRows + kNormalRows);
                if (other != kNoPalette) {
                    blend_palette(&g_replay_rows[palette_at], &g_previous.rows[other], palette, t);
                }
            }
        }
        uint32_t at = static_cast<uint32_t>(g_replay_rows.size());
        g_replay_rows.insert(g_replay_rows.end(), now, now + run.draw.rows);
        Row* out = &g_replay_rows[at];
        if (before) {
            blend_run(out, then, now, t, run.draw.key);
        }
        if (run.draw.palette != kNoPalette) {
            out[3][2] = static_cast<float>(static_cast<int>(palette_at) - static_cast<int>(at));
        }
        RunDraw draw = run.draw;
        draw.row = at;
        g_replay_draws.push_back(draw);
    }
    return send_replayed();
}

enum PairKind : uint8_t { kNoPair, kCountChanged, kSameCount, kOnly };

template <typename Body>
void parallel_for(size_t count, const Body& body) {
    struct Pool {
        std::mutex mutex;
        std::condition_variable wake;
        std::condition_variable done;
        const std::function<void(size_t)>* job = nullptr;
        size_t count = 0;
        std::atomic<size_t> next{0};
        uint64_t generation = 0;
        size_t working = 0;
        size_t helpers = 0;
    };
    constexpr size_t kChunk = 64;
    static Pool* pool = [] {
        Pool* made = new Pool;
        size_t cores = std::thread::hardware_concurrency();
        made->helpers = cores > 4 ? std::min<size_t>(3, cores - 3) : 0;
        for (size_t h = 0; h < made->helpers; h++) {
            std::thread([made] {
                uint64_t seen = 0;
                std::unique_lock<std::mutex> lock(made->mutex);
                while (true) {
                    made->wake.wait(lock, [&] { return made->generation != seen; });
                    seen = made->generation;
                    const std::function<void(size_t)>* job = made->job;
                    size_t count = made->count;
                    lock.unlock();
                    for (size_t i = made->next.fetch_add(kChunk); i < count; i = made->next.fetch_add(kChunk)) {
                        for (size_t j = i; j < std::min(count, i + kChunk); j++) {
                            (*job)(j);
                        }
                    }
                    lock.lock();
                    if (--made->working == 0) {
                        made->done.notify_all();
                    }
                }
            }).detach();
        }
        return made;
    }();
    std::function<void(size_t)> job = body;
    if (pool->helpers == 0 || count < 256) {
        for (size_t i = 0; i < count; i++) {
            job(i);
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lock(pool->mutex);
        pool->job = &job;
        pool->count = count;
        pool->next = 0;
        pool->working = pool->helpers;
        pool->generation++;
    }
    pool->wake.notify_all();
    for (size_t i = pool->next.fetch_add(kChunk); i < count; i = pool->next.fetch_add(kChunk)) {
        for (size_t j = i; j < std::min(count, i + kChunk); j++) {
            job(j);
        }
    }
    std::unique_lock<std::mutex> lock(pool->mutex);
    pool->done.wait(lock, [&] { return pool->working == 0; });
}

void match_by(uint64_t RecordedDraw::*key, std::vector<uint32_t>& partner, std::vector<uint8_t>& taken, std::vector<uint8_t>& kind,
              uint32_t& moved_too_far) {
    static std::vector<std::pair<uint32_t, uint32_t>> candidates;
    static std::vector<uint8_t> close;
    candidates.clear();
    struct Slot {
        uint64_t key = 0;
        uint32_t next = ~0u;
        uint32_t before = 0;
        uint32_t now = 0;
    };
    static std::vector<Slot> table;
    static std::vector<uint32_t> next_same;
    size_t size = 1;
    while (size < (g_previous.draws.size() + g_record.draws.size()) * 2) {
        size <<= 1;
    }
    table.assign(size, Slot{});
    auto slot = [&](uint64_t value) -> Slot& {
        size_t i = (value * 0x9E3779B97F4A7C15ull) >> 20 & (size - 1);
        while (table[i].key != 0 && table[i].key != value) {
            i = (i + 1) & (size - 1);
        }
        table[i].key = value;
        return table[i];
    };
    next_same.assign(g_previous.draws.size(), ~0u);
    for (size_t i = g_previous.draws.size(); i-- > 0;) {
        uint64_t value = g_previous.draws[i].*key;
        if (value == 0 || taken[i]) {
            continue;
        }
        Slot& entry = slot(value);
        next_same[i] = entry.next;
        entry.next = static_cast<uint32_t>(i);
        entry.before++;
    }
    for (size_t d = 0; d < g_record.draws.size(); d++) {
        if (partner[d] == kUnmatched && g_record.draws[d].*key != 0) {
            slot(g_record.draws[d].*key).now++;
        }
    }
    for (size_t d = 0; d < g_record.draws.size(); d++) {
        const RecordedDraw& draw = g_record.draws[d];
        if (partner[d] != kUnmatched || draw.*key == 0) {
            continue;
        }
        Slot& entry = slot(draw.*key);
        uint32_t p = entry.next;
        if (p == ~0u) {
            continue;
        }
        entry.next = next_same[p];
        kind[d] = entry.before != entry.now ? kCountChanged : (entry.now == 1 ? kOnly : kSameCount);
        const RecordedDraw& before = g_previous.draws[p];
        if (before.positions != draw.positions || draw.vertices == 0) {
            continue;
        }
        candidates.emplace_back(static_cast<uint32_t>(d), p);
    }
    close.assign(candidates.size(), 0);
    parallel_for(candidates.size(), [](size_t i) {
        close[i] = close_draw(g_previous.draws[candidates[i].second], g_record.draws[candidates[i].first]);
    });
    for (size_t i = 0; i < candidates.size(); i++) {
        auto [d, p] = candidates[i];
        if (!close[i]) {
            moved_too_far++;
            partner[d] = kRejected;
            continue;
        }
        partner[d] = p;
        taken[p] = 1;
        if (kind[d] != kCountChanged) {
            g_color_partner[d] = p;
        }
    }
}

uint32_t g_match[6] = {};

void find_cameras(const std::vector<uint32_t>& partner, const std::vector<uint8_t>& kind) {
    for (CameraMotion& camera : g_cameras) {
        camera.claimed = false;
        camera.known = false;
        camera.second = kMaxCameras;
        camera.is_second = false;
        camera.reach = 0.0f;
        camera.from.clear();
        camera.to.clear();
    }
    constexpr size_t kMaxSamples = 8192;
    constexpr size_t kThreePoints = 2048;
    for (size_t d = 0; d < partner.size(); d++) {
        if (partner[d] >= kRejected || kind[d] != kOnly) {
            continue;
        }
        const RecordedDraw& draw = g_record.draws[d];
        const RecordedDraw& before = g_previous.draws[partner[d]];
        CameraMotion* camera = nullptr;
        for (CameraMotion& candidate : g_cameras) {
            if (!candidate.claimed) {
                candidate.claimed = true;
                candidate.id = draw.camera;
            }
            if (candidate.id == draw.camera) {
                camera = &candidate;
                break;
            }
        }
        if (!camera || camera->from.size() >= kMaxSamples || draw.positions == 0) {
            continue;
        }
        bool three = camera->from.size() < kThreePoints;
        for (uint32_t i : {draw.positions / 2, 0u, draw.positions - 1}) {
            const std::array<float, 4>& a = g_previous.positions[before.first_position + i];
            const std::array<float, 4>& b = g_record.positions[draw.first_position + i];
            if (!std::isnan(a[0]) && !std::isnan(b[0])) {
                camera->from.push_back(b);
                camera->to.push_back(a);
            }
            if (!three) {
                break;
            }
        }
    }
    for (CameraMotion& camera : g_cameras) {
        for (size_t i = 0; i < camera.from.size(); i++) {
            const std::array<float, 4>& b = camera.from[i];
            const std::array<float, 4>& a = camera.to[i];
            if (a[3] > 0.0f && b[3] > 0.0f) {
                camera.reach = std::max(camera.reach, std::max(std::fabs(a[0] / a[3] - b[0] / b[3]), std::fabs(a[1] / a[3] - b[1] / b[3])));
            }
        }
        camera.known = camera.claimed && gx::fit_motion(reinterpret_cast<const float(*)[4]>(camera.from.data()), reinterpret_cast<const float(*)[4]>(camera.to.data()),
                                                        camera.from.size(), camera.back);
    }
    static std::vector<std::array<float, 4>> rest_from;
    static std::vector<std::array<float, 4>> rest_to;
    for (size_t k = 0; k < kMaxCameras; k++) {
        CameraMotion& camera = g_cameras[k];
        if (!camera.known || camera.is_second) {
            continue;
        }
        size_t free = kMaxCameras;
        for (size_t j = 0; j < kMaxCameras; j++) {
            if (!g_cameras[j].claimed) {
                free = j;
                break;
            }
        }
        if (free == kMaxCameras) {
            break;
        }
        rest_from.clear();
        rest_to.clear();
        for (size_t i = 0; i < camera.from.size(); i++) {
            const std::array<float, 4>& b = camera.from[i];
            const std::array<float, 4>& a = camera.to[i];
            if (a[3] <= 0.0f || b[3] <= 0.0f) {
                continue;
            }
            double p[4];
            for (int r = 0; r < 4; r++) {
                p[r] = static_cast<double>(camera.back[r][0]) * b[0] + static_cast<double>(camera.back[r][1]) * b[1] + static_cast<double>(camera.back[r][2]) * b[2] +
                       static_cast<double>(camera.back[r][3]) * b[3];
            }
            if (p[3] <= 0.0 || std::max(std::fabs(p[0] / p[3] - a[0] / a[3]), std::fabs(p[1] / p[3] - a[1] / a[3])) > kStillScreen) {
                rest_from.push_back(b);
                rest_to.push_back(a);
            }
        }
        constexpr size_t kMinimumRest = 96;
        CameraMotion& other = g_cameras[free];
        if (rest_from.size() < kMinimumRest ||
            !gx::fit_motion(reinterpret_cast<const float(*)[4]>(rest_from.data()), reinterpret_cast<const float(*)[4]>(rest_to.data()), rest_from.size(), other.back)) {
            continue;
        }
        other.claimed = true;
        other.known = true;
        other.is_second = true;
        other.id = camera.id;
        other.reach = camera.reach;
        camera.second = free;
    }
}

size_t camera_of(uint32_t id) {
    for (size_t k = 0; k < kMaxCameras; k++) {
        if (g_cameras[k].known && g_cameras[k].id == id) {
            return k;
        }
    }
    return kMaxCameras;
}

bool near_camera_path(const RecordedDraw& draw, const RecordedDraw& before, const CameraMotion& camera) {
    const std::array<float, 4>& a = g_previous.positions[before.first_position];
    const std::array<float, 4>& b = g_record.positions[draw.first_position];
    if (std::isnan(a[0]) || std::isnan(b[0])) {
        return false;
    }
    float p[4];
    for (int r = 0; r < 4; r++) {
        p[r] = camera.back[r][0] * b[0] + camera.back[r][1] * b[1] + camera.back[r][2] * b[2] + camera.back[r][3] * b[3];
    }
    constexpr float kNear = 0.05f;
    return p[3] > 0.0f && a[3] > 0.0f && std::fabs(p[0] / p[3] - a[0] / a[3]) <= kNear && std::fabs(p[1] / p[3] - a[1] / a[3]) <= kNear;
}

bool camera_moves_sanely(const RecordedDraw& draw, const CameraMotion& camera, float least = 0.04f) {
    constexpr uint32_t kChecked = 9;
    constexpr float kParallax = 4.0f;
    constexpr float kDepthSlack = 0.25f;
    constexpr float kDepthFloor = 1e-5f;
    const float limit = std::min(kMaxStep, std::max(least, kParallax * camera.reach));
    uint32_t step = std::max(1u, draw.positions / kChecked);
    for (uint32_t i = 0; i < draw.positions; i += step) {
        const std::array<float, 4>& b = g_record.positions[draw.first_position + i];
        if (std::isnan(b[0]) || b[3] <= 0.0f) {
            continue;
        }
        float p[4];
        for (int r = 0; r < 4; r++) {
            p[r] = camera.back[r][0] * b[0] + camera.back[r][1] * b[1] + camera.back[r][2] * b[2] + camera.back[r][3] * b[3];
        }
        if (!(p[3] > 0.0f) || std::fabs(p[0] / p[3] - b[0] / b[3]) > limit || std::fabs(p[1] / p[3] - b[1] / b[3]) > limit) {
            return false;
        }
        float gap = (1.0f - b[2] / b[3]) * b[3] / p[3];
        if (gap > 0.0f && std::fabs(1.0f - p[2] / p[3] - gap) > kDepthSlack * gap + kDepthFloor) {
            return false;
        }
    }
    return true;
}

bool fixed_on_screen(const RecordedDraw& draw, const RecordedDraw& before) {
    return draw.positions != 0 && draw.positions == before.positions &&
           std::memcmp(&g_record.positions[draw.first_position], &g_previous.positions[before.first_position], draw.positions * sizeof(std::array<float, 4>)) == 0;
}

std::vector<std::pair<uint32_t, uint32_t>> g_matches;
bool g_camera_cut = false;

void follow_decals(const std::vector<uint32_t>& partner, const std::vector<uint8_t>& kind, const std::vector<uint8_t>& column, std::vector<uint32_t>& target) {
    constexpr size_t kMaxGroup = 24;
    constexpr float kSame = 1e-3f;
    constexpr float kMaxMove = 0.1f;
    g_decal_shift.assign(partner.size(), {0.0f, 0.0f});
    static std::vector<uint8_t> grouped;
    static std::vector<uint32_t> group;
    static std::vector<std::array<float, 2>> before;
    static std::vector<std::array<float, 2>> now;
    grouped.assign(partner.size(), 0);
    auto candidate = [&](size_t d) { return !grouped[d] && column[d] == 2 && kind[d] == kSameCount && partner[d] < kRejected; };
    for (size_t first = 0; first < partner.size(); first++) {
        if (!candidate(first)) {
            continue;
        }
        uint32_t id = g_record.draws[first].camera;
        group.clear();
        before.clear();
        now.clear();
        bool flat = true;
        for (size_t d = first; d < partner.size(); d++) {
            if (!candidate(d) || g_record.draws[d].camera != id) {
                continue;
            }
            grouped[d] = 1;
            group.push_back(static_cast<uint32_t>(d));
            const std::array<float, 4>& a = g_previous.positions[g_previous.draws[partner[d]].first_position];
            const std::array<float, 4>& b = g_record.positions[g_record.draws[d].first_position];
            flat = flat && a[3] == 1.0f && b[3] == 1.0f && !std::isnan(a[0]) && !std::isnan(b[0]);
            before.push_back({a[0], a[1]});
            now.push_back({b[0], b[1]});
        }
        size_t count = group.size();
        if (!flat || count < 2 || count > kMaxGroup) {
            continue;
        }
        auto rests = [&](size_t k, float x, float y) {
            for (size_t l = 0; l < count; l++) {
                if (std::fabs(now[k][0] - before[l][0] - x) < kSame && std::fabs(now[k][1] - before[l][1] - y) < kSame) {
                    return true;
                }
            }
            return false;
        };
        size_t best = 0;
        float shift[2] = {};
        for (size_t i = 0; i < count; i++) {
            for (size_t j = 0; j < count; j++) {
                float x = now[i][0] - before[j][0];
                float y = now[i][1] - before[j][1];
                size_t support = 0;
                for (size_t k = 0; k < count; k++) {
                    support += rests(k, x, y);
                }
                if (support > best) {
                    best = support;
                    shift[0] = x;
                    shift[1] = y;
                }
            }
        }
        if (best * 2 < count) {
            continue;
        }
        for (size_t k = 0; k < count; k++) {
            if (rests(k, shift[0], shift[1]) || std::fabs(now[k][0] - before[k][0] - shift[0]) > kMaxMove || std::fabs(now[k][1] - before[k][1] - shift[1]) > kMaxMove) {
                continue;
            }
            target[group[k]] = partner[group[k]];
            g_decal_shift[group[k]] = {shift[0], shift[1]};
        }
    }
}

void pair_frame() {
    auto started = std::chrono::steady_clock::now();
    static std::vector<uint32_t> partner;
    static std::vector<uint8_t> taken;
    partner.assign(g_record.draws.size(), kUnmatched);
    g_color_partner.assign(g_record.draws.size(), kUnmatched);
    taken.assign(g_previous.draws.size(), 0);
    uint32_t moved_too_far = 0;
    static std::vector<uint8_t> kind;
    kind.assign(g_record.draws.size(), kNoPair);
    match_by(&RecordedDraw::exact, partner, taken, kind, moved_too_far);
    match_by(&RecordedDraw::key, partner, taken, kind, moved_too_far);
    find_cameras(partner, kind);
    adopt_run_camera();
    static bool forward[kMaxCameras];
    for (size_t k = 0; k < kMaxCameras; k++) {
        forward[k] = g_cameras[k].known && invert(g_cameras[k].back, g_forward[k]);
    }
    g_world_camera.assign(g_record.draws.size(), static_cast<uint8_t>(kMaxCameras));
    g_world_moving.assign(g_record.draws.size(), 0);
    constexpr uint8_t kNoDraw = 255;
    static std::vector<uint32_t> target;
    static std::vector<uint8_t> column;
    target.assign(partner.size(), kUnmatched);
    column.assign(partner.size(), 0);
    parallel_for(partner.size(), [](size_t d) {
        const RecordedDraw& draw = g_record.draws[d];
        if (draw.vertices == 0) {
            column[d] = kNoDraw;
            return;
        }
        bool pair = partner[d] < kRejected;
        size_t camera = camera_of(draw.camera);
        if (pair && camera < kMaxCameras && forward[camera] && camera_moves_sanely(draw, g_cameras[camera])) {
            g_world_camera[d] = static_cast<uint8_t>(camera);
            g_world_moving[d] = moves_in_world(draw, g_previous.draws[partner[d]], g_cameras[camera]);
        }
        if (pair && camera < kMaxCameras && kind[d] != kOnly && fixed_on_screen(draw, g_previous.draws[partner[d]])) {
            target[d] = partner[d];
            column[d] = 1;
        } else if (camera < kMaxCameras && !(pair && (kind[d] == kOnly || (kind[d] == kSameCount && near_camera_path(draw, g_previous.draws[partner[d]], g_cameras[camera]))))) {
            constexpr float kLeastForNewDraw = 0.004f;
            bool sane = camera_moves_sanely(draw, g_cameras[camera], kLeastForNewDraw);
            target[d] = sane ? kByCamera + static_cast<uint32_t>(camera) : kUnmatched;
            column[d] = sane ? 5 : 2;
        } else if (pair && camera == kMaxCameras && kind[d] != kOnly) {
            column[d] = 2;
        } else if (pair) {
            target[d] = partner[d];
            column[d] = kind[d] == kOnly ? 0 : 1;
        } else {
            column[d] = partner[d] == kRejected ? 2 : (draw.exact != 0 ? 3 : 4);
        }
    });
    follow_decals(partner, kind, column, target);
    g_matches.clear();
    for (size_t d = 0; d < partner.size(); d++) {
        if (column[d] == kNoDraw) {
            continue;
        }
        g_match[column[d]]++;
        if (target[d] != kUnmatched) {
            g_matches.emplace_back(static_cast<uint32_t>(d), target[d]);
        }
    }
    g_camera_cut = moved_too_far > 8 && moved_too_far * 3 > g_matches.size();
    g_time_pairing += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
}

bool g_paired = false;

bool can_pair() {
    return g_recording && !g_record_broken && ((!g_previous.draws.empty() && !g_record.draws.empty()) || (!g_previous.runs.empty() && !g_record.runs.empty()));
}

constexpr std::array<float, 4> kOwnColor = {0.0f, 0.0f, 0.0f, -1.0f};
constexpr MovedVertex kKeep = {{0.0f, 0.0f, 0.0f, -1e30f}, {kOwnColor, kOwnColor}};

void motion_matrices(float t, float (&motion)[kMaxCameras][4][4]) {
    for (size_t k = 0; k < kMaxCameras; k++) {
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                motion[k][r][c] = g_cameras[k].back[r][c] * (1.0f - t) + (r == c ? t : 0.0f);
            }
        }
    }
}

void fill_moved(MovedVertex* moved, const std::vector<std::pair<uint32_t, uint32_t>>& matches, float t, const float (&motion)[kMaxCameras][4][4]) {
    size_t written = 0;
    for (const auto& [d, p] : matches) {
        const RecordedDraw& draw = g_record.draws[d];
        std::fill(moved + written, moved + draw.first_vertex, kKeep);
        size_t camera = p >= kByCamera ? p - kByCamera : g_world_camera[d];
        if (p >= kByCamera || (camera < kMaxCameras && !g_world_moving[d])) {
            written = draw.first_vertex + draw.vertices;
            std::fill(moved + draw.first_vertex, moved + written, MovedVertex{{static_cast<float>(camera), 0.0f, 0.0f, -2e30f}, {kOwnColor, kOwnColor}});
            continue;
        }
        const RecordedDraw& before = g_previous.draws[p];
        if (camera < kMaxCameras) {
            const double (*forward)[4] = g_forward[camera];
            const float (*now)[4] = motion[camera];
            size_t second = g_cameras[camera].second;
            for (uint32_t v = 0; v < draw.vertices; v++) {
                size_t index = draw.first_vertex + v;
                uint16_t source = g_record.source_of[index];
                const std::array<float, 4>* a = source == kNoSource ? nullptr : &g_previous.positions[before.first_position + source];
                const std::array<float, 4>* b = source == kNoSource ? nullptr : &g_record.positions[draw.first_position + source];
                if (!a || std::isnan((*a)[0]) || std::isnan((*b)[0])) {
                    moved[index] = kKeep;
                    continue;
                }
                size_t follow = stillness(*a, *b, g_cameras[camera]) != kMoved ? camera
                                : second < kMaxCameras && stillness(*a, *b, g_cameras[second]) == kStill ? second : kMaxCameras;
                if (follow < kMaxCameras) {
                    moved[index] = {{static_cast<float>(follow), 0.0f, 0.0f, -2e30f}, {kOwnColor, kOwnColor}};
                    continue;
                }
                double world[4];
                for (int r = 0; r < 4; r++) {
                    double seen = forward[r][0] * (*a)[0] + forward[r][1] * (*a)[1] + forward[r][2] * (*a)[2] + forward[r][3] * (*a)[3];
                    world[r] = seen + ((*b)[r] - seen) * t;
                }
                moved[index].color[0] = kOwnColor;
                moved[index].color[1] = kOwnColor;
                for (int r = 0; r < 4; r++) {
                    moved[index].position[r] = static_cast<float>(now[r][0] * world[0] + now[r][1] * world[1] + now[r][2] * world[2] + now[r][3] * world[3]);
                }
            }
            written = draw.first_vertex + draw.vertices;
            continue;
        }
        for (uint32_t v = 0; v < draw.vertices; v++) {
            size_t index = draw.first_vertex + v;
            uint16_t source = g_record.source_of[index];
            const std::array<float, 4>* a = source == kNoSource ? nullptr : &g_previous.positions[before.first_position + source];
            const std::array<float, 4>* b = source == kNoSource ? nullptr : &g_record.positions[draw.first_position + source];
            if (!a || std::isnan((*a)[0]) || std::isnan((*b)[0])) {
                moved[index] = kKeep;
                continue;
            }
            float ax = (*a)[0] + g_decal_shift[d][0];
            float ay = (*a)[1] + g_decal_shift[d][1];
            moved[index] = {{ax + ((*b)[0] - ax) * t, ay + ((*b)[1] - ay) * t, (*a)[2] + ((*b)[2] - (*a)[2]) * t, (*a)[3] + ((*b)[3] - (*a)[3]) * t}, {kOwnColor, kOwnColor}};
        }
        written = draw.first_vertex + draw.vertices;
    }
    std::fill(moved + written, moved + g_frame_vertices, kKeep);
    for (size_t d = 0; d < g_record.draws.size() && d < g_color_partner.size(); d++) {
        const RecordedDraw& draw = g_record.draws[d];
        uint32_t p = g_color_partner[d];
        if (p >= kRejected || p >= g_previous.draws.size() || g_previous.draws[p].positions != draw.positions || draw.positions == 0 ||
            g_previous.draws[p].material != draw.material) {
            continue;
        }
        const RecordedDraw& before = g_previous.draws[p];
        for (uint32_t v = 0; v < draw.vertices && draw.first_vertex + v < g_frame_vertices; v++) {
            size_t index = draw.first_vertex + v;
            uint16_t source = g_record.source_of[index];
            if (source == kNoSource) {
                continue;
            }
            const auto& a = g_previous.colors[before.first_position + source];
            const auto& b = g_record.colors[draw.first_position + source];
            if (a == b) {
                continue;
            }
            for (int c = 0; c < 2; c++) {
                float alpha = a[c][3] + (b[c][3] - a[c][3]) * t;
                for (int k = 0; k < 3; k++) {
                    moved[index].color[c][k] = alpha > 0.0f ? (a[c][k] * a[c][3] + (b[c][k] * b[c][3] - a[c][k] * a[c][3]) * t) / alpha : b[c][k];
                }
                moved[index].color[c][3] = alpha;
            }
        }
    }
}

const std::vector<uint32_t>& command_partners() {
    static std::vector<uint32_t> command_partner;
    static uint64_t partnered_frame = ~0ull;
    if (partnered_frame != g_frame) {
        partnered_frame = g_frame;
        command_partner.assign(g_record.commands.size(), ~0u);
        static std::vector<uint32_t> lists[2];
        lists[0].clear();
        lists[1].clear();
        for (uint32_t i = 0; i < g_previous_colors.size(); i++) {
            lists[g_previous_colors[i].runs].push_back(i);
        }
        auto previous_command = [](bool runs, size_t at) {
            const std::vector<uint32_t>& list = lists[runs];
            auto found = std::upper_bound(list.begin(), list.end(), at, [](size_t value, uint32_t i) { return value < g_previous_colors[i].begin; });
            if (found == list.begin()) {
                return ~0u;
            }
            uint32_t i = *(found - 1);
            return at < g_previous_colors[i].end ? i : ~0u;
        };
        size_t d = 0;
        for (size_t c = 0; c < g_record.commands.size(); c++) {
            const RecordedCommand& command = g_record.commands[c];
            if (command.kind == RecordedCommand::kRuns) {
                for (size_t r = command.begin; r < command.end; r++) {
                    if (g_record.runs[r].match != kUnmatched) {
                        command_partner[c] = previous_command(true, g_record.runs[r].match);
                        break;
                    }
                }
            } else if (command.kind == RecordedCommand::kBatch) {
                while (d < g_record.draws.size() && g_record.draws[d].first_vertex < command.begin) {
                    d++;
                }
                for (size_t e = d; e < g_record.draws.size() && g_record.draws[e].first_vertex < command.end; e++) {
                    uint32_t p = e < g_color_partner.size() ? g_color_partner[e] : kUnmatched;
                    if (p < kRejected && p < g_previous.draws.size() && g_previous.draws[p].material == g_record.draws[e].material) {
                        command_partner[c] = previous_command(false, g_previous.draws[p].first_vertex);
                        break;
                    }
                }
            }
        }
    }
    return command_partner;
}

const BatchState* blend_colors(const RecordedCommand& command, uint32_t partner, float t, BatchState& blended) {
    const BatchState* state = &command.batch;
    if (partner != ~0u) {
        const CommandColors& before = g_previous_colors[partner];
        const Constants& now = command.batch.constants;
        if ((std::memcmp(before.initial, now.initial, sizeof before.initial) != 0 || std::memcmp(before.konst, now.konst, sizeof before.konst) != 0) &&
            before.shape == command_shape(command.batch)) {
            blended = command.batch;
            for (int i = 0; i < 4; i++) {
                for (int k = 0; k < 4; k++) {
                    blended.constants.initial[i][k] = before.initial[i][k] + (now.initial[i][k] - before.initial[i][k]) * t;
                    blended.constants.konst[i][k] = before.konst[i][k] + (now.konst[i][k] - before.konst[i][k]) * t;
                }
            }
            state = &blended;
        }
    }
    return state;
}

void keep_colors() {
    for (const RecordedCommand& command : g_record.commands) {
        if (command.kind == RecordedCommand::kBatch || command.kind == RecordedCommand::kRuns) {
            CommandColors colors;
            colors.runs = command.kind == RecordedCommand::kRuns;
            colors.begin = command.begin;
            colors.end = command.end;
            colors.shape = command_shape(command.batch);
            std::memcpy(colors.initial, command.batch.constants.initial, sizeof colors.initial);
            std::memcpy(colors.konst, command.batch.constants.konst, sizeof colors.konst);
            g_previous_colors.push_back(colors);
        }
    }
}
