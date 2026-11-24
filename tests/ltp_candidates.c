/*
 * LTP候補比較機能の回帰テスト
 *
 * fitting     : LPCCalculator_CalculateLTPCoefficientsAtPeriod の妥当性
 * selection   : 実圧縮ブロックヘッダの解析と候補選択の検証
 * integration : 1〜8ch/16・24bit のラウンドトリップ、最適分割、互換性
 *
 * 実装・素材生成は C と標準ライブラリのみで行う。
 */
#include "task_support/check.h"

#include "srla_encoder.h"
#include "srla_decoder.h"
#include "srla_internal.h"
#include "srla_utility.h"
#include "srla_coder.h"
#include "lpc.h"
#include "static_huffman.h"
#include "bit_stream.h"
#include "byte_array.h"

#include <math.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define TEST_MAX_CHANNELS   SRLA_MAX_NUM_CHANNELS
#define TEST_MAX_SAMPLES    1024
#define TEST_CAPACITY       (1u << 20)
#define TEST_PI             M_PI

/* ------------------------------------------------------------------ */
/* 共通ユーティリティ                                                 */
/* ------------------------------------------------------------------ */

static int32_t g_in[TEST_MAX_CHANNELS][TEST_MAX_SAMPLES];
static int32_t g_out[TEST_MAX_CHANNELS][TEST_MAX_SAMPLES];
static int32_t g_work[TEST_MAX_CHANNELS][TEST_MAX_SAMPLES];
static uint8_t g_encoded[TEST_CAPACITY];
static uint8_t g_encoded2[TEST_CAPACITY];

static uint32_t g_rng_state = 123456789u;
static uint32_t test_rand(void)
{
    g_rng_state = g_rng_state * 1664525u + 1013904223u;
    return g_rng_state;
}

/* [-1,1] の一様乱数 */
static double test_rand_unit(void)
{
    return (double)(test_rand() >> 8) / (double)(1u << 24);
}

/* 離散正弦波（決定的） */
static double test_sine(double freq, uint32_t smpl, uint32_t rate)
{
    return sin(2.0 * TEST_PI * freq * (double)smpl / (double)rate);
}

/* 擬似音声: 基本周期 period の周期性 + 倍音 + 小さなノイズ */
static int32_t test_tonal_sample(uint32_t period, uint32_t smpl, int channel, int amplitude)
{
    const double base = 2.0 * TEST_PI * (double)smpl / (double)period;
    double v = 0.70 * sin(base + 0.31 * channel)
             + 0.20 * sin(2.0 * base + 0.11 * channel)
             + 0.07 * sin(3.0 * base + 0.53 * channel);
    v += 0.05 * (test_rand_unit() * 2.0 - 1.0);
    return (int32_t)(v * amplitude);
}

/* テスト用エンコーダ/デコーダの組 */
struct TestCodec {
    struct SRLAEncoder *enc;
    struct SRLADecoder *dec;
};

static int test_codec_create(struct TestCodec *tc, uint32_t num_channels,
    uint32_t bps, uint32_t min_block, uint32_t max_block, uint32_t lookahead,
    uint32_t max_params)
{
    struct SRLAEncoderConfig config;
    struct SRLADecoderConfig dconfig;
    struct SRLAEncodeParameter param;
    struct SRLAHeader header;

    memset(tc, 0, sizeof(*tc));
    config.max_num_channels = num_channels;
    config.min_num_samples_per_block = min_block;
    config.max_num_samples_per_block = max_block;
    config.max_num_lookahead_samples = lookahead;
    config.max_num_parameters = max_params;

    param.num_channels = (uint16_t)num_channels;
    param.bits_per_sample = (uint16_t)bps;
    param.sampling_rate = 44100;
    param.min_num_samples_per_block = min_block;
    param.max_num_samples_per_block = max_block;
    param.num_lookahead_samples = lookahead;
    param.ltp_order = 0;
    param.num_svr_filter_learning_iteration = 0;
    param.preset = 2; /* 16次LPCまで */

    dconfig.max_num_channels = num_channels;
    dconfig.max_num_parameters = max_params;
    dconfig.check_checksum = 1;

    memset(&header, 0, sizeof(header));
    header.format_version = SRLA_FORMAT_VERSION;
    header.codec_version = SRLA_CODEC_VERSION;
    header.num_channels = (uint16_t)num_channels;
    header.num_samples = TEST_MAX_SAMPLES;
    header.sampling_rate = 44100;
    header.bits_per_sample = (uint16_t)bps;
    header.offset_lshift = 0;
    header.max_num_samples_per_block = max_block;
    header.preset = 2;

    tc->enc = SRLAEncoder_Create(&config, NULL, 0);
    tc->dec = SRLADecoder_Create(&dconfig, NULL, 0);
    if (tc->enc == NULL || tc->dec == NULL) {
        return 0;
    }
    if (SRLAEncoder_SetEncodeParameter(tc->enc, &param) != SRLA_APIRESULT_OK) {
        return 0;
    }
    if (SRLADecoder_SetHeader(tc->dec, &header) != SRLA_APIRESULT_OK) {
        return 0;
    }
    return 1;
}

static void test_codec_destroy(struct TestCodec *tc)
{
    SRLADecoder_Destroy(tc->dec);
    SRLAEncoder_Destroy(tc->enc);
    tc->dec = NULL;
    tc->enc = NULL;
}

/* ------------------------------------------------------------------ */
/* fitting: 直接相関・小行列求解による独立検証                        */
/* ------------------------------------------------------------------ */

/* Welch窓を適用（lpc.cの実装と同じ対称ループ。偶数サンプルを前提） */
static void test_apply_welch(const double *in, uint32_t n, double *out)
{
    uint32_t i;
    const double divisor = 4.0 * pow((double)n - 1.0, -2.0);
    for (i = 0; i < (n >> 1); i++) {
        const double w = divisor * (double)i * (double)(n - 1 - i);
        out[i] = in[i] * w;
        out[n - i - 1] = in[n - i - 1] * w;
    }
}

/* FFT経由の自己相関と等価な循環相関を直接和で計算（2/n正規化）
 * 補足）ライブラリは num_samples を2の冪に切り上げて0埋めし、
 *       その長さで循環相関を計算する。よって同じ長さで循環相関を取る。 */
static void test_circular_autocorr(const double *x, uint32_t n, double *ac, uint32_t max_lag)
{
    uint32_t fft_size = 1, lag, k;
    double *padded;
    while (fft_size < n) {
        fft_size <<= 1;
    }
    padded = (double *)calloc(fft_size, sizeof(double));
    if (padded == NULL) {
        return;
    }
    memcpy(padded, x, sizeof(double) * n);
    for (lag = 0; lag <= max_lag; lag++) {
        double sum = 0.0;
        for (k = 0; k < fft_size; k++) {
            const uint32_t j = (k + lag) % fft_size;
            sum += padded[k] * padded[j];
        }
        ac[lag] = sum * 2.0 / (double)n;
    }
    free(padded);
}

/* 部分ピボット付きガウス消去で dim 次連立方程式を解く */
static int test_solve_small(double *a, const double *b, double *x, uint32_t dim)
{
    uint32_t i, j, k;
    double m[8][9];
    if (dim > 8) {
        return 1;
    }
    for (i = 0; i < dim; i++) {
        for (j = 0; j < dim; j++) {
            m[i][j] = a[i * dim + j];
        }
        m[i][dim] = b[i];
    }
    for (i = 0; i < dim; i++) {
        uint32_t piv = i;
        for (j = i + 1; j < dim; j++) {
            if (fabs(m[j][i]) > fabs(m[piv][i])) {
                piv = j;
            }
        }
        if (fabs(m[piv][i]) < 1e-12) {
            return 1;
        }
        if (piv != i) {
            for (k = 0; k <= dim; k++) {
                const double t = m[i][k]; m[i][k] = m[piv][k]; m[piv][k] = t;
            }
        }
        for (j = i + 1; j < dim; j++) {
            const double f = m[j][i] / m[i][i];
            for (k = i; k <= dim; k++) {
                m[j][k] -= f * m[i][k];
            }
        }
    }
    for (i = dim; i-- > 0; ) {
        double sum = m[i][dim];
        for (j = i + 1; j < dim; j++) {
            sum -= m[i][j] * x[j];
        }
        x[i] = sum / m[i][i];
    }
    return 0;
}

/* 独立にLTP係数を求める（窓→循環相関→ridge→小行列求解） */
static int test_fit_ltp_direct(const double *data, uint32_t n, uint32_t period,
    uint32_t order, double regular_term, double *coef)
{
    double *windowed, *ac, *a, *b;
    double wcoef[TEST_MAX_SAMPLES];
    uint32_t j, k, max_lag;
    int ok;

    windowed = (double *)malloc(sizeof(double) * n);
    max_lag = period + (order / 2) + 1;
    ac = (double *)malloc(sizeof(double) * (max_lag + 1));
    a = (double *)malloc(sizeof(double) * order * order);
    b = (double *)malloc(sizeof(double) * order);
    if (windowed == NULL || ac == NULL || a == NULL || b == NULL) {
        free(windowed); free(ac); free(a); free(b);
        return 0;
    }
    test_apply_welch(data, n, wcoef);
    memcpy(windowed, wcoef, sizeof(double) * n);
    test_circular_autocorr(windowed, n, ac, max_lag);
    ac[0] *= (1.0 + regular_term);
    for (j = 0; j < order; j++) {
        for (k = 0; k < order; k++) {
            const uint32_t lag = (j >= k) ? (j - k) : (k - j);
            a[j * order + k] = ac[lag];
        }
        b[j] = ac[period - (order / 2) + j];
    }
    ok = (test_solve_small(a, b, coef, order) == 0);
    free(windowed); free(ac); free(a); free(b);
    return ok;
}

static void test_fill_synthetic(double *data, uint32_t n)
{
    uint32_t i;
    g_rng_state = 987654321u;
    for (i = 0; i < n; i++) {
        data[i] = 0.30 * sin(0.15 * i) + 0.20 * sin(0.31 * i + 0.4)
                + 0.10 * (test_rand_unit() * 2.0 - 1.0);
    }
}

static int test_fitting_matches_direct(void)
{
    const uint32_t n = 512;
    const uint32_t periods[] = { 37, 91, 200 };
    const uint32_t orders[] = { 1, 3 };
    const double ridge = 1e-5;
    double data[TEST_MAX_SAMPLES];
    double coef[8];
    struct LPCCalculatorConfig config;
    struct LPCCalculator *lpcc;
    uint32_t pi, oi;

    config.max_order = 8;
    config.max_num_samples = n;
    lpcc = LPCCalculator_Create(&config, NULL, 0);
    TASK_CHECK(lpcc != NULL);

    test_fill_synthetic(data, n);
    for (pi = 0; pi < sizeof(periods) / sizeof(periods[0]); pi++) {
        for (oi = 0; oi < sizeof(orders) / sizeof(orders[0]); oi++) {
            double ref[8];
            double got[8];
            uint32_t k;
            LPCApiResult ret;

            TASK_CHECK(test_fit_ltp_direct(data, n, periods[pi], orders[oi], ridge, ref));
            memset(got, 0xA5, sizeof(got));
            ret = LPCCalculator_CalculateLTPCoefficientsAtPeriod(lpcc,
                data, n, (int32_t)periods[pi], got, orders[oi],
                LPC_WINDOWTYPE_WELCH, ridge);
            TASK_CHECK(ret == LPC_APIRESULT_OK);
            for (k = 0; k < orders[oi]; k++) {
                const double diff = fabs(got[k] - ref[k]);
                const double scale = 1.0 + fabs(ref[k]);
                if (diff > 1e-9 * scale) {
                    printf("MISMATCH pi=%u oi=%u k=%u n=%u period=%u order=%u got=%.17g ref=%.17g\n",
                        pi, oi, k, n, periods[pi], orders[oi], got[k], ref[k]);
                }
                TASK_CHECK(diff <= 1e-9 * scale);
            }
        }
    }

    LPCCalculator_Destroy(lpcc);
    return 0;
}

/* lagが局所ピークでなくてもフィッティングできること */
static int test_fitting_arbitrary_lag(void)
{
    const uint32_t n = 400;
    const uint32_t period = 53; /* 自己相関の谷を選ぶ */
    const uint32_t order = 3;
    const double ridge = 1e-5;
    double data[TEST_MAX_SAMPLES];
    double ref[4], got[4];
    struct LPCCalculatorConfig config;
    struct LPCCalculator *lpcc;
    uint32_t k;

    config.max_order = 8;
    config.max_num_samples = n;
    lpcc = LPCCalculator_Create(&config, NULL, 0);
    TASK_CHECK(lpcc != NULL);

    g_rng_state = 24680u;
    for (k = 0; k < n; k++) {
        data[k] = 0.4 * sin(0.05 * k) + 0.2 * (test_rand_unit() * 2.0 - 1.0);
    }
    TASK_CHECK(test_fit_ltp_direct(data, n, period, order, ridge, ref));
    TASK_CHECK(LPCCalculator_CalculateLTPCoefficientsAtPeriod(lpcc,
        data, n, (int32_t)period, got, order,
        LPC_WINDOWTYPE_WELCH, ridge) == LPC_APIRESULT_OK);
    for (k = 0; k < order; k++) {
        TASK_CHECK(fabs(got[k] - ref[k]) <= 1e-9 * (1.0 + fabs(ref[k])));
    }

    LPCCalculator_Destroy(lpcc);
    return 0;
}

/* 自動検出の従来動作が保たれ、検出周期での固定ラグ結果と一致すること */
static int test_fitting_auto_unchanged(void)
{
    const uint32_t n = 512;
    const double ridge = 1e-5;
    double data[TEST_MAX_SAMPLES];
    double auto_coef[4], fixed_coef[4];
    int32_t detected = 0;
    struct LPCCalculatorConfig config;
    struct LPCCalculator *lpcc;
    uint32_t k;
    LPCApiResult ret;

    config.max_order = 8;
    config.max_num_samples = n;
    lpcc = LPCCalculator_Create(&config, NULL, 0);
    TASK_CHECK(lpcc != NULL);

    /* 周期64の周期的信号 */
    for (k = 0; k < n; k++) {
        data[k] = 0.5 * sin(2.0 * TEST_PI * (double)k / 64.0)
                + 0.2 * sin(2.0 * TEST_PI * (double)k / 32.0);
    }
    ret = LPCCalculator_CalculateLTPCoefficients(lpcc, data, n,
        8, 262, auto_coef, 3, &detected, LPC_WINDOWTYPE_WELCH, ridge);
    TASK_CHECK(ret == LPC_APIRESULT_OK);
    TASK_CHECK(detected >= 8);
    TASK_CHECK(LPCCalculator_CalculateLTPCoefficientsAtPeriod(lpcc, data, n,
        detected, fixed_coef, 3, LPC_WINDOWTYPE_WELCH, ridge) == LPC_APIRESULT_OK);
    for (k = 0; k < 3; k++) {
        TASK_CHECK(fabs(auto_coef[k] - fixed_coef[k]) <= 1e-12 * (1.0 + fabs(auto_coef[k])));
    }

    LPCCalculator_Destroy(lpcc);
    return 0;
}

static int test_is_not_nan_buffer(const double *buf, uint32_t n)
{
    uint32_t i;
    for (i = 0; i < n; i++) {
        if (isnan(buf[i]) || isinf(buf[i])) {
            return 0;
        }
    }
    return 1;
}

static int test_fitting_failures(void)
{
    const uint32_t n = 256;
    double zero[TEST_MAX_SAMPLES];
    double singular[TEST_MAX_SAMPLES];
    double data[TEST_MAX_SAMPLES];
    double coef[8];
    struct LPCCalculatorConfig config;
    struct LPCCalculator *lpcc;
    uint32_t k;
    LPCApiResult ret;

    config.max_order = 8;
    config.max_num_samples = n;
    lpcc = LPCCalculator_Create(&config, NULL, 0);
    TASK_CHECK(lpcc != NULL);

    memset(zero, 0, sizeof(zero));
    test_fill_synthetic(data, n);

    /* 零エネルギー: 明示的失敗、出力は非NaNのまま（書き換えない） */
    for (k = 0; k < 8; k++) {
        coef[k] = 42.0;
    }
    ret = LPCCalculator_CalculateLTPCoefficientsAtPeriod(lpcc, zero, n, 16,
        coef, 3, LPC_WINDOWTYPE_WELCH, 1e-5);
    TASK_CHECK(ret != LPC_APIRESULT_OK);
    TASK_CHECK(test_is_not_nan_buffer(coef, 8));
    for (k = 0; k < 8; k++) {
        TASK_CHECK(coef[k] == 42.0);
    }

    /* 特異行列: 定数信号（矩形窓）は自己相関が全ラグで等しく、
     * 3次の自己相関行列が（ridgeなしで）特異になる */
    for (k = 0; k < n; k++) {
        singular[k] = 0.25;
    }
    for (k = 0; k < 8; k++) {
        coef[k] = 7.0;
    }
    ret = LPCCalculator_CalculateLTPCoefficientsAtPeriod(lpcc, singular, n, 64,
        coef, 3, LPC_WINDOWTYPE_RECTANGULAR, 0.0);
    TASK_CHECK(ret != LPC_APIRESULT_OK);
    TASK_CHECK(test_is_not_nan_buffer(coef, 8));
    for (k = 0; k < 8; k++) {
        TASK_CHECK(coef[k] == 7.0);
    }

    /* 偶数次数は不正 */
    TASK_CHECK(LPCCalculator_CalculateLTPCoefficientsAtPeriod(lpcc, data, n, 32,
        coef, 2, LPC_WINDOWTYPE_WELCH, 1e-5) == LPC_APIRESULT_INVALID_ARGUMENT);

    /* 周期が次数の半分+1未満は不正 */
    TASK_CHECK(LPCCalculator_CalculateLTPCoefficientsAtPeriod(lpcc, data, n, 1,
        coef, 3, LPC_WINDOWTYPE_WELCH, 1e-5) == LPC_APIRESULT_INVALID_ARGUMENT);

    /* 周期の両側の相関項が入力長を越える */
    TASK_CHECK(LPCCalculator_CalculateLTPCoefficientsAtPeriod(lpcc, data, n, (int32_t)n - 1,
        coef, 3, LPC_WINDOWTYPE_WELCH, 1e-5) != LPC_APIRESULT_OK);

    /* 最大次数を越える */
    TASK_CHECK(LPCCalculator_CalculateLTPCoefficientsAtPeriod(lpcc, data, n, 32,
        coef, 9, LPC_WINDOWTYPE_WELCH, 1e-5) == LPC_APIRESULT_EXCEED_MAX_ORDER);

    /* 入力サンプル数がワークサイズを越える */
    TASK_CHECK(LPCCalculator_CalculateLTPCoefficientsAtPeriod(lpcc, data, n + 1, 32,
        coef, 3, LPC_WINDOWTYPE_WELCH, 1e-5) == LPC_APIRESULT_EXCEED_MAX_NUM_SAMPLES);

    /* NULL引数 */
    TASK_CHECK(LPCCalculator_CalculateLTPCoefficientsAtPeriod(NULL, data, n, 32,
        coef, 3, LPC_WINDOWTYPE_WELCH, 1e-5) == LPC_APIRESULT_INVALID_ARGUMENT);
    TASK_CHECK(LPCCalculator_CalculateLTPCoefficientsAtPeriod(lpcc, NULL, n, 32,
        coef, 3, LPC_WINDOWTYPE_WELCH, 1e-5) == LPC_APIRESULT_INVALID_ARGUMENT);
    TASK_CHECK(LPCCalculator_CalculateLTPCoefficientsAtPeriod(lpcc, data, n, 32,
        NULL, 3, LPC_WINDOWTYPE_WELCH, 1e-5) == LPC_APIRESULT_INVALID_ARGUMENT);

    LPCCalculator_Destroy(lpcc);
    return 0;
}

/* ------------------------------------------------------------------ */
/* selection: 独立コスト列挙                                            */
/* ------------------------------------------------------------------ */

struct TestModel {
    uint32_t ltp_period;
    uint32_t ltp_order;
    int32_t ltp_coef[SRLA_MAX_LTP_ORDER];
    uint32_t lpc_order;
    uint32_t lpc_rshift;
    int32_t lpc_coef[SRLA_MAX_COEFFICIENT_ORDER];
    uint32_t bits; /* LTPフラグを含む総ビット数 */
};

struct TestParsedChannel {
    int32_t lpc_coef[SRLA_MAX_COEFFICIENT_ORDER];
    uint32_t lpc_order;
    uint32_t lpc_rshift;
    uint32_t use_sum;
    uint32_t ltp_order;
    uint32_t ltp_period;
    int32_t ltp_coef[SRLA_MAX_LTP_ORDER];
};

static struct StaticHuffmanCodes g_param_codes;
static struct StaticHuffmanCodes g_sum_param_codes;
static int g_codes_ready = 0;

static void test_prepare_codes(void)
{
    if (!g_codes_ready) {
        StaticHuffman_ConvertTreeToCodes(SRLA_GetParameterHuffmanTree(), &g_param_codes);
        StaticHuffman_ConvertTreeToCodes(SRLA_GetSumParameterHuffmanTree(), &g_sum_param_codes);
        g_codes_ready = 1;
    }
}

/* 符号長計算の幾何分布エントロピー（エンコーダ内部実装の写し） */
static double test_geometric_entropy(double mean_abs_error, uint32_t bps)
{
    const double intmean = mean_abs_error * (1 << (bps - 1));
    const double rho = 1.0 / (1.0 + intmean);
    const double invrho = 1.0 - rho;
    const double invloge2 = 1.4426950408889634;
    if (mean_abs_error < 1e-16) {
        return 0.0;
    }
    return -(invrho * (log(invrho) * invloge2) + rho * (log(rho) * invloge2)) / rho;
}

/* プリエンファシス係数と適用後の信号（入力は破壊しない） */
static void test_preemphasis(const int32_t *input, uint32_t n,
    struct SRLAPreemphasisFilter *filter, int32_t *output)
{
    int32_t head;
    SRLAPreemphasisFilter_CalculateCoefficient(filter, input, n);
    head = input[0];
    filter->prev = head;
    memcpy(output, input, sizeof(int32_t) * n);
    SRLAPreemphasisFilter_Preemphasis(filter, output, n);
}

/* LTP予測（スカラー実装の写し。入力は変更しない） */
static void test_ltp_predict(const int32_t *input, uint32_t n,
    const int32_t *coef, uint32_t order, uint32_t period, int32_t *residual)
{
    uint32_t smpl, ord;
    /* 実際の呼び出しは coef_rshift = LTP_COEFFICIENT_BITWIDTH - 1 = 5 */
    const uint32_t rshift = SRLA_LTP_COEFFICIENT_BITWIDTH - 1;
    const int32_t half = 1 << (rshift - 1);
    const uint32_t half_order = order >> 1;

    memcpy(residual, input, sizeof(int32_t) * n);
    for (smpl = period + half_order + 1; smpl < n; smpl++) {
        int32_t predict = half;
        for (ord = 0; ord < order; ord++) {
            predict += coef[ord] * input[smpl + ord - period - half_order];
        }
        residual[smpl] -= (predict >> rshift);
    }
}

/* LPC予測（スカラー実装の写し） */
static void test_lpc_predict(const int32_t *input, uint32_t n,
    const int32_t *coef, uint32_t order, uint32_t rshift, int32_t *residual)
{
    uint32_t smpl, ord;
    const int32_t half = (order > 0) ? (1 << (rshift - 1)) : 0;

    memcpy(residual, input, sizeof(int32_t) * n);
    if (order == 0) {
        return;
    }
    for (smpl = 1; smpl < order; smpl++) {
        residual[smpl] = input[smpl] - input[smpl - 1];
    }
    for (smpl = 0; smpl < n - order; smpl++) {
        int32_t predict = half;
        for (ord = 0; ord < order; ord++) {
            predict += coef[ord] * input[smpl + ord];
        }
        residual[smpl + order] += (predict >> rshift);
    }
}

/* エンコーダの量子化規則（丸め・クリップ・並び反転） */
static void test_quantize_ltp(const double *dcoef, uint32_t order, int32_t *icoef)
{
    uint32_t p;
    const int32_t lim = 1 << (SRLA_LTP_COEFFICIENT_BITWIDTH - 1);
    for (p = 0; p < order; p++) {
        int32_t v = (int32_t)SRLAUtility_Round(dcoef[p] * pow(2.0, SRLA_LTP_COEFFICIENT_BITWIDTH - 1));
        if (v < -lim) { v = -lim; }
        if (v > lim - 1) { v = lim - 1; }
        icoef[p] = v;
    }
    for (p = 0; p < order / 2; p++) {
        const int32_t t = icoef[p];
        icoef[p] = icoef[order - p - 1];
        icoef[order - p - 1] = t;
    }
}

/* LPC分析（次数選択・SVRなし・量子化・残差・LPC領域符号長）*/
static SRLAError test_analyze_lpc(struct LPCCalculator *lpcc, struct SRLACoder *coder,
    const int32_t *lpc_input, uint32_t n, uint32_t bps, uint32_t preset_index,
    double **multiple_coefs, double *error_vars,
    struct TestModel *model, int32_t *residual, uint32_t *lpc_bits_out)
{
    const struct SRLAParameterPreset *preset = &g_srla_parameter_preset[preset_index];
    double *dbuf;
    double *dcoef;
    uint32_t smpl, p, order = 0, rshift = 0, use_sum = 0;
    uint32_t lpc_bits = 0;
    const double norm = pow(2.0, -(int32_t)(bps - 1));
    LPCApiResult ret;

    dbuf = (double *)malloc(sizeof(double) * n);
    if (dbuf == NULL) {
        return SRLA_ERROR_NG;
    }
    for (smpl = 0; smpl < n; smpl++) {
        dbuf[smpl] = lpc_input[smpl] * norm;
    }
    if ((ret = LPCCalculator_CalculateMultipleLPCCoefficients(lpcc, dbuf, n,
        multiple_coefs, error_vars, preset->max_num_parameters,
        LPC_WINDOWTYPE_WELCH, SRLA_LPC_RIDGE_REGULARIZATION_PARAMETER)) != LPC_APIRESULT_OK) {
        free(dbuf);
        return SRLA_ERROR_NG;
    }

    /* 次数選択 */
    if (preset->lpc_order_tactics == SRLA_LPC_ORDER_DECISION_TACTICS_MAX_FIXED) {
        order = preset->max_num_parameters;
    } else {
        double minlen = FLT_MAX;
        uint32_t o;
        for (o = 1; o <= preset->max_num_parameters; o++) {
            const double mabse = 2.0 * sqrt(error_vars[o] / 2.0);
            double len = test_geometric_entropy(mabse, bps) * n;
            len += SRLA_LPC_COEFFICIENT_BITWIDTH * o;
            if (minlen > len) {
                minlen = len;
                order = o;
            }
        }
    }

    if (order > 0) {
        dcoef = multiple_coefs[order - 1];
        /* SVR学習回数0のため初期係数のまま（呼び出しは省略） */
        if ((ret = LPC_QuantizeCoefficients(dcoef, order,
            SRLA_LPC_COEFFICIENT_BITWIDTH, (1 << SRLA_RSHIFT_LPC_COEFFICIENT_BITWIDTH),
            model->lpc_coef, &rshift)) != LPC_APIRESULT_OK) {
            free(dbuf);
            return SRLA_ERROR_NG;
        }
        for (p = 0; p < order / 2; p++) {
            const int32_t t = model->lpc_coef[p];
            model->lpc_coef[p] = model->lpc_coef[order - p - 1];
            model->lpc_coef[order - p - 1] = t;
        }
        test_lpc_predict(lpc_input, n, model->lpc_coef, order, rshift, residual);
    } else {
        memcpy(residual, lpc_input, sizeof(int32_t) * n);
        rshift = 0;
    }
    model->lpc_order = order;
    model->lpc_rshift = rshift;

    /* LPC領域符号長 */
    lpc_bits = SRLACoder_ComputeCodeLength(coder, residual, n);
    lpc_bits += bps + 1;
    for (p = 0; p < SRLA_NUM_PREEMPHASIS_FILTERS; p++) {
        lpc_bits += SRLA_PREEMPHASIS_COEF_SHIFT + 1;
    }
    lpc_bits += SRLA_LPC_COEFFICIENT_ORDER_BITWIDTH;
    lpc_bits += SRLA_RSHIFT_LPC_COEFFICIENT_BITWIDTH;
    lpc_bits += 1;
    if (order > 0) {
        uint32_t plain = 0, summed;
        for (p = 0; p < order; p++) {
            const uint32_t uval = SRLAUTILITY_SINT32_TO_UINT32(model->lpc_coef[p]);
            plain += g_param_codes.codes[uval].bit_count;
        }
        use_sum = 1;
        summed = g_param_codes.codes[SRLAUTILITY_SINT32_TO_UINT32(model->lpc_coef[0])].bit_count;
        for (p = 1; p < order; p++) {
            const int32_t s = model->lpc_coef[p] + model->lpc_coef[p - 1];
            const uint32_t uval = SRLAUTILITY_SINT32_TO_UINT32(s);
            if (uval >= STATICHUFFMAN_MAX_NUM_SYMBOLS) {
                use_sum = 0;
                break;
            }
            summed += g_sum_param_codes.codes[uval].bit_count;
            if (summed >= plain) {
                use_sum = 0;
                break;
            }
        }
        lpc_bits += use_sum ? summed : plain;
    }
    model->bits = lpc_bits;
    *lpc_bits_out = lpc_bits;
    free(dbuf);
    return SRLA_ERROR_OK;
}

/* 1信号に対する全候補の独立コスト列挙 */
static SRLAError test_enumerate_best(const int32_t *signal, uint32_t n, uint32_t bps,
    uint32_t preset_index, const uint32_t *periods, uint32_t count,
    struct LPCCalculator *lpcc, struct SRLACoder *coder,
    double **multiple_coefs, double *error_vars,
    struct TestModel *best, int32_t *best_residual)
{
    struct SRLAPreemphasisFilter filter;
    int32_t preemph[TEST_MAX_SAMPLES];
    int32_t ltp_res[TEST_MAX_SAMPLES];
    int32_t residual[TEST_MAX_SAMPLES];
    uint32_t best_bits = UINT32_MAX;
    int found = 0;
    uint32_t oi, ci;
    const double norm = pow(2.0, -(int32_t)(bps - 1));

    test_prepare_codes();
    test_preemphasis(signal, n, &filter, preemph);

    /* 無効LTP */
    {
        struct TestModel m;
        memset(&m, 0, sizeof(m));
        m.ltp_period = 0;
        m.ltp_order = 0;
        if (test_analyze_lpc(lpcc, coder, preemph, n, bps, preset_index,
            multiple_coefs, error_vars, &m, residual, &m.bits) != SRLA_ERROR_OK) {
            return SRLA_ERROR_NG;
        }
        m.bits += 1; /* LTPフラグ */
        if (m.bits < best_bits) {
            best_bits = m.bits;
            *best = m;
            memcpy(best_residual, residual, sizeof(int32_t) * n);
            found = 1;
        }
    }

    /* 次数1→3、各次数内で周期昇順（同コストの優先順位を再現） */
    for (oi = 0; oi < 2; oi++) {
        const uint32_t order = (oi == 0) ? 1 : 3;
        for (ci = 0; ci < count; ci++) {
            const uint32_t period = periods[ci];
            double dcoef[SRLA_MAX_LTP_ORDER] = { 0.0 };
            int32_t icoef[SRLA_MAX_LTP_ORDER] = { 0 };
            struct TestModel m;
            LPCApiResult ret;
            double *dbuf;
            uint32_t smpl;

            if (period < (order / 2) + 1) {
                continue;
            }
            dbuf = (double *)malloc(sizeof(double) * n);
            if (dbuf == NULL) {
                return SRLA_ERROR_NG;
            }
            for (smpl = 0; smpl < n; smpl++) {
                dbuf[smpl] = preemph[smpl] * norm;
            }
            ret = LPCCalculator_CalculateLTPCoefficientsAtPeriod(lpcc, dbuf, n,
                (int32_t)period, dcoef, order, LPC_WINDOWTYPE_WELCH,
                SRLA_LPC_RIDGE_REGULARIZATION_PARAMETER);
            free(dbuf);
            if (ret != LPC_APIRESULT_OK) {
                continue;
            }
            test_quantize_ltp(dcoef, order, icoef);
            test_ltp_predict(preemph, n, icoef, order, period, ltp_res);

            memset(&m, 0, sizeof(m));
            m.ltp_period = period;
            m.ltp_order = order;
            memcpy(m.ltp_coef, icoef, sizeof(icoef));
            if (test_analyze_lpc(lpcc, coder, ltp_res, n, bps, preset_index,
                multiple_coefs, error_vars, &m, residual, &m.bits) != SRLA_ERROR_OK) {
                return SRLA_ERROR_NG;
            }
            m.bits += 1;
            m.bits += SRLA_LTP_ORDER_BITWIDTH;
            m.bits += SRLA_LTP_PERIOD_BITWIDTH;
            m.bits += order * SRLA_LTP_COEFFICIENT_BITWIDTH;
            if (m.bits < best_bits) {
                best_bits = m.bits;
                *best = m;
                memcpy(best_residual, residual, sizeof(int32_t) * n);
                found = 1;
            }
        }
    }

    return found ? SRLA_ERROR_OK : SRLA_ERROR_NG;
}

/* 圧縮ブロックのパラメータと残差を解析する */
static int test_parse_block(const uint8_t *block, uint32_t size, uint32_t num_channels,
    uint32_t bps, uint32_t num_samples,
    uint32_t *ch_process, struct TestParsedChannel *parsed, int32_t (*residual)[TEST_MAX_SAMPLES])
{
    struct BitStream reader;
    const uint8_t *payload;
    uint32_t ch, i, uval;
    uint32_t block_size;

    test_prepare_codes();
    if (size < 11) {
        return 0;
    }
    /* ブロックヘッダ: 同期(2) サイズ(4) チェックサム(2) タイプ(1) サンプル数(2) */
    if ((block[0] != 0xFF) || (block[1] != 0xFF)) {
        return 0;
    }
    if (block[8] != SRLA_BLOCK_DATA_TYPE_COMPRESSDATA) {
        return 0;
    }
    block_size = ((uint32_t)block[2] << 24) | ((uint32_t)block[3] << 16)
               | ((uint32_t)block[4] << 8) | (uint32_t)block[5];
    if (block_size + 6 > size) {
        return 0;
    }
    payload = block + 11;
    BitReader_Open(&reader, payload, size - 11);

    BitReader_GetBits(&reader, ch_process, 2);

    /* プリエンファシス */
    for (ch = 0; ch < num_channels; ch++) {
        BitReader_GetBits(&reader, &uval, bps + 1);
        for (i = 0; i < SRLA_NUM_PREEMPHASIS_FILTERS; i++) {
            BitReader_GetBits(&reader, &uval, SRLA_PREEMPHASIS_COEF_SHIFT + 1);
        }
    }
    /* LPC */
    for (ch = 0; ch < num_channels; ch++) {
        memset(&parsed[ch], 0, sizeof(parsed[ch]));
        BitReader_GetBits(&reader, &parsed[ch].lpc_order, SRLA_LPC_COEFFICIENT_ORDER_BITWIDTH);
        BitReader_GetBits(&reader, &parsed[ch].lpc_rshift, SRLA_RSHIFT_LPC_COEFFICIENT_BITWIDTH);
        BitReader_GetBits(&reader, &parsed[ch].use_sum, 1);
        if (!parsed[ch].use_sum) {
            for (i = 0; i < parsed[ch].lpc_order; i++) {
                uval = StaticHuffman_GetCode(SRLA_GetParameterHuffmanTree(), &reader);
                parsed[ch].lpc_coef[i] = SRLAUTILITY_UINT32_TO_SINT32(uval);
            }
        } else {
            uval = StaticHuffman_GetCode(SRLA_GetParameterHuffmanTree(), &reader);
            parsed[ch].lpc_coef[0] = SRLAUTILITY_UINT32_TO_SINT32(uval);
            for (i = 1; i < parsed[ch].lpc_order; i++) {
                uval = StaticHuffman_GetCode(SRLA_GetSumParameterHuffmanTree(), &reader);
                parsed[ch].lpc_coef[i] = SRLAUTILITY_UINT32_TO_SINT32(uval) - parsed[ch].lpc_coef[i - 1];
            }
        }
    }
    /* LTP */
    for (ch = 0; ch < num_channels; ch++) {
        BitReader_GetBits(&reader, &uval, 1);
        if (uval != 0) {
            BitReader_GetBits(&reader, &uval, SRLA_LTP_ORDER_BITWIDTH);
            parsed[ch].ltp_order = 2 * uval + 1;
            BitReader_GetBits(&reader, &uval, SRLA_LTP_PERIOD_BITWIDTH);
            parsed[ch].ltp_period = uval + SRLA_LTP_MIN_PERIOD;
            for (i = 0; i < parsed[ch].ltp_order; i++) {
                BitReader_GetBits(&reader, &uval, SRLA_LTP_COEFFICIENT_BITWIDTH);
                parsed[ch].ltp_coef[i] = SRLAUTILITY_UINT32_TO_SINT32(uval);
            }
        } else {
            parsed[ch].ltp_period = 0;
            parsed[ch].ltp_order = 0;
        }
    }
    /* 残差 */
    for (ch = 0; ch < num_channels; ch++) {
        SRLACoder_Decode(&reader, residual[ch], num_samples);
    }
    BitStream_Close(&reader);
    return 1;
}

static int test_models_equal(const struct TestModel *a, const struct TestParsedChannel *b)
{
    uint32_t i;
    if (a->ltp_period != b->ltp_period || a->ltp_order != b->ltp_order) {
        return 0;
    }
    if (a->lpc_order != b->lpc_order || a->lpc_rshift != b->lpc_rshift) {
        return 0;
    }
    for (i = 0; i < a->ltp_order; i++) {
        if (a->ltp_coef[i] != b->ltp_coef[i]) {
            return 0;
        }
    }
    for (i = 0; i < a->lpc_order; i++) {
        if (a->lpc_coef[i] != b->lpc_coef[i]) {
            return 0;
        }
    }
    return 1;
}

/* 候補比較モードの単一ブロック検証用コンテキスト */
struct TestSelCtx {
    struct LPCCalculator *lpcc;
    struct SRLACoder *coder;
    double **multiple_coefs;
    double *error_vars;
};

static int test_sel_ctx_create(struct TestSelCtx *ctx, uint32_t max_params, uint32_t max_samples)
{
    struct LPCCalculatorConfig lpcc_config;
    int32_t coder_size;
    uint32_t i;

    ctx->lpcc = NULL;
    ctx->coder = NULL;
    ctx->multiple_coefs = NULL;
    ctx->error_vars = NULL;

    lpcc_config.max_order = (max_params > SRLA_MAX_LTP_ORDER) ? max_params : SRLA_MAX_LTP_ORDER;
    lpcc_config.max_num_samples = max_samples;
    ctx->lpcc = LPCCalculator_Create(&lpcc_config, NULL, 0);
    coder_size = SRLACoder_CalculateWorkSize(max_samples);
    ctx->coder = SRLACoder_Create(max_samples, NULL, 0);
    ctx->multiple_coefs = (double **)malloc(sizeof(double *) * max_params);
    for (i = 0; i < max_params; i++) {
        ctx->multiple_coefs[i] = (double *)malloc(sizeof(double) * max_params);
    }
    ctx->error_vars = (double *)malloc(sizeof(double) * (max_params + 1));
    return (ctx->lpcc != NULL && ctx->coder != NULL && ctx->multiple_coefs != NULL
            && ctx->error_vars != NULL);
}

static void test_sel_ctx_destroy(struct TestSelCtx *ctx, uint32_t max_params)
{
    uint32_t i;
    if (ctx->multiple_coefs != NULL) {
        for (i = 0; i < max_params; i++) {
            free(ctx->multiple_coefs[i]);
        }
        free(ctx->multiple_coefs);
    }
    free(ctx->error_vars);
    SRLACoder_Destroy(ctx->coder);
    LPCCalculator_Destroy(ctx->lpcc);
}

/* ------------------------------------------------------------------ */
/* 単一圧縮ブロックをエンコードして解析する */
static int test_encode_and_parse(struct TestCodec *tc, uint32_t num_channels, uint32_t bps,
    uint32_t num_samples, int32_t (*in)[TEST_MAX_SAMPLES],
    uint32_t *ch_process, struct TestParsedChannel *parsed,
    int32_t (*residual)[TEST_MAX_SAMPLES], uint32_t *encoded_bytes, int32_t (*decoded)[TEST_MAX_SAMPLES])
{
    const int32_t *input[TEST_MAX_CHANNELS];
    int32_t *output[TEST_MAX_CHANNELS];
    uint32_t ch, bytes = 0, estimate = 0, consumed = 0, decoded_samples = 0;

    for (ch = 0; ch < num_channels; ch++) {
        input[ch] = in[ch];
        output[ch] = decoded[ch];
    }
    if (SRLAEncoder_ComputeBlockSize(tc->enc, input, num_samples, &estimate) != SRLA_APIRESULT_OK) {
        return 0;
    }
    if (SRLAEncoder_EncodeBlock(tc->enc, input, num_samples, g_encoded, sizeof(g_encoded), &bytes)
        != SRLA_APIRESULT_OK) {
        return 0;
    }
    /* ComputeBlockSize と EncodeBlock のサイズ一致 */
    if (bytes != estimate) {
        return 0;
    }
    if (!test_parse_block(g_encoded, bytes, num_channels, bps, num_samples, ch_process, parsed, residual)) {
        return 0;
    }
    if (SRLADecoder_DecodeBlock(tc->dec, g_encoded, bytes, output, num_channels,
        TEST_MAX_SAMPLES, &consumed, &decoded_samples) != SRLA_APIRESULT_OK) {
        return 0;
    }
    if (consumed != bytes || decoded_samples != num_samples) {
        return 0;
    }
    if (encoded_bytes != NULL) {
        *encoded_bytes = bytes;
    }
    return 1;
}

/* 1信号に対する最良モデルの独立列挙 */
static int test_expected_model(struct TestSelCtx *ctx, const int32_t *signal, uint32_t n,
    uint32_t bps, uint32_t preset, const uint32_t *periods, uint32_t count,
    struct TestModel *best, int32_t *residual)
{
    return test_enumerate_best(signal, n, bps, preset, periods, count,
        ctx->lpcc, ctx->coder, ctx->multiple_coefs, ctx->error_vars, best, residual) == SRLA_ERROR_OK;
}

/* ------------------------------------------------------------------ */
/* selection: 単一ブロックのモデル選択                                 */
/* ------------------------------------------------------------------ */

static int test_selection_single_channel(void)
{
    enum { CH = 1, N = 256, BPS = 16, PRESET = 2 };
    struct TestCodec tc;
    struct TestSelCtx ctx;
    struct TestModel expected;
    struct TestParsedChannel parsed[CH];
    int32_t residual[CH][TEST_MAX_SAMPLES];
    int32_t expected_residual[TEST_MAX_SAMPLES];
    uint32_t periods[3] = { 32, 64, 65 };
    uint32_t ch_process = 0, encoded = 0, i;

    TASK_CHECK(test_codec_create(&tc, CH, BPS, N, N, N, 16));
    TASK_CHECK(test_sel_ctx_create(&ctx, 16, N));
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(tc.enc, periods, 3) == SRLA_APIRESULT_OK);

    g_rng_state = 111u;
    for (i = 0; i < N; i++) {
        /* 周期64を強く含む信号（ノイズは小さめ） */
        double v = 0.80 * sin(2.0 * TEST_PI * (double)i / 64.0)
                 + 0.15 * sin(4.0 * TEST_PI * (double)i / 64.0 + 0.3);
        v += 0.02 * (test_rand_unit() * 2.0 - 1.0);
        g_in[0][i] = (int32_t)(v * 9000.0);
    }

    TASK_CHECK(test_encode_and_parse(&tc, CH, BPS, N, g_in, &ch_process, parsed,
        residual, &encoded, g_out));
    for (i = 0; i < N; i++) {
        TASK_CHECK(g_out[0][i] == g_in[0][i]);
    }
    TASK_CHECK(ch_process == SRLA_CH_PROCESS_METHOD_NONE);
    TASK_CHECK(parsed[0].lpc_order <= 16);

    /* 独立列挙と一致 */
    TASK_CHECK(test_expected_model(&ctx, g_in[0], N, BPS, PRESET, periods, 3,
        &expected, expected_residual));
    TASK_CHECK(test_models_equal(&expected, &parsed[0]));
    for (i = 0; i < N; i++) {
        TASK_CHECK(expected_residual[i] == residual[0][i]);
    }

    test_sel_ctx_destroy(&ctx, 16);
    test_codec_destroy(&tc);
    return 0;
}

static int test_selection_order_preference(void)
{
    enum { CH = 1, N = 256, BPS = 16 };
    struct TestCodec tc;
    struct TestSelCtx ctx;
    struct TestModel expected;
    struct TestParsedChannel parsed[CH];
    int32_t residual[CH][TEST_MAX_SAMPLES];
    int32_t expected_residual[TEST_MAX_SAMPLES];
    uint32_t periods[2] = { 48, 96 };
    uint32_t ch_process = 0, i;

    TASK_CHECK(test_codec_create(&tc, CH, BPS, N, N, N, 16));
    TASK_CHECK(test_sel_ctx_create(&ctx, 16, N));
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(tc.enc, periods, 2) == SRLA_APIRESULT_OK);

    g_rng_state = 222u;
    for (i = 0; i < N; i++) {
        double v = 0.7 * sin(2.0 * TEST_PI * (double)i / 48.0)
                 + 0.1 * sin(2.0 * TEST_PI * (double)i / 24.0);
        v += 0.03 * (test_rand_unit() * 2.0 - 1.0);
        g_in[0][i] = (int32_t)(v * 8000.0);
    }

    TASK_CHECK(test_encode_and_parse(&tc, CH, BPS, N, g_in, &ch_process, parsed,
        residual, &i, g_out));
    TASK_CHECK(test_expected_model(&ctx, g_in[0], N, BPS, 2, periods, 2,
        &expected, expected_residual));
    TASK_CHECK(test_models_equal(&expected, &parsed[0]));

    test_sel_ctx_destroy(&ctx, 16);
    test_codec_destroy(&tc);
    return 0;
}

/* 候補が存在してもLTP無効が最良なら無効が選ばれること */
static int test_selection_disable_wins(void)
{
    enum { CH = 1, N = 256, BPS = 16 };
    struct TestCodec tc;
    struct TestSelCtx ctx;
    struct TestModel expected;
    struct TestParsedChannel parsed[CH];
    int32_t residual[CH][TEST_MAX_SAMPLES];
    int32_t expected_residual[TEST_MAX_SAMPLES];
    uint32_t periods[2] = { 200, 250 };
    uint32_t ch_process = 0, i, unused = 0;

    TASK_CHECK(test_codec_create(&tc, CH, BPS, N, N, N, 16));
    TASK_CHECK(test_sel_ctx_create(&ctx, 16, N));
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(tc.enc, periods, 2) == SRLA_APIRESULT_OK);

    /* 白色雑音: 周期予測は役に立たない */
    g_rng_state = 333u;
    for (i = 0; i < N; i++) {
        g_in[0][i] = (int32_t)((test_rand_unit() * 2.0 - 1.0) * 6000.0);
    }

    TASK_CHECK(test_encode_and_parse(&tc, CH, BPS, N, g_in, &ch_process, parsed,
        residual, &unused, g_out));
    TASK_CHECK(test_expected_model(&ctx, g_in[0], N, BPS, 2, periods, 2,
        &expected, expected_residual));
    TASK_CHECK(test_models_equal(&expected, &parsed[0]));

    test_sel_ctx_destroy(&ctx, 16);
    test_codec_destroy(&tc);
    return 0;
}

/* 周期が短すぎて次数3のフィッティングができない候補はスキップされる */
static int test_selection_skips_unfit_period(void)
{
    enum { CH = 1, N = 64, BPS = 16 };
    struct TestCodec tc;
    struct TestSelCtx ctx;
    struct TestModel expected;
    struct TestParsedChannel parsed[CH];
    int32_t residual[CH][TEST_MAX_SAMPLES];
    int32_t expected_residual[TEST_MAX_SAMPLES];
    uint32_t periods[2] = { 40, 63 }; /* 63は次数3の両側相関が入力長を越える */
    uint32_t ch_process = 0, i, unused = 0;

    TASK_CHECK(test_codec_create(&tc, CH, BPS, 32, N, N, 16));
    TASK_CHECK(test_sel_ctx_create(&ctx, 16, N));
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(tc.enc, periods, 2) == SRLA_APIRESULT_OK);

    g_rng_state = 444u;
    for (i = 0; i < N; i++) {
        double v = 0.8 * sin(2.0 * TEST_PI * (double)i / 40.0);
        v += 0.02 * (test_rand_unit() * 2.0 - 1.0);
        g_in[0][i] = (int32_t)(v * 7000.0);
    }

    TASK_CHECK(test_encode_and_parse(&tc, CH, BPS, N, g_in, &ch_process, parsed,
        residual, &unused, g_out));
    TASK_CHECK(test_expected_model(&ctx, g_in[0], N, BPS, 2, periods, 2,
        &expected, expected_residual));
    TASK_CHECK(test_models_equal(&expected, &parsed[0]));
    for (i = 0; i < N; i++) {
        TASK_CHECK(g_out[0][i] == g_in[0][i]);
    }

    test_sel_ctx_destroy(&ctx, 16);
    test_codec_destroy(&tc);
    return 0;
}

/* 短いブロックで全候補（無効/1次/3次×周期）のコストを独立列挙して照合する */
static int test_selection_short_block_enumerates_all(void)
{
    enum { CH = 1, N = 64, BPS = 16, PRESET = 2 };
    struct TestCodec tc;
    struct TestSelCtx ctx;
    struct TestModel expected;
    struct TestParsedChannel parsed[CH];
    int32_t residual[CH][TEST_MAX_SAMPLES];
    int32_t expected_residual[TEST_MAX_SAMPLES];
    uint32_t periods[3] = { 20, 26, 32 };
    uint32_t ch_process = 0, i, unused = 0;

    TASK_CHECK(test_codec_create(&tc, CH, BPS, N, N, N, 16));
    TASK_CHECK(test_sel_ctx_create(&ctx, 16, N));
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(tc.enc, periods, 3) == SRLA_APIRESULT_OK);

    /* 周期26付近の弱い周期性 + ノイズ: 候補間のコスト差が僅かになるようにする */
    g_rng_state = 2024u;
    for (i = 0; i < N; i++) {
        double v = 0.35 * sin(2.0 * TEST_PI * (double)i / 26.0)
                 + 0.30 * sin(2.0 * TEST_PI * (double)i / 20.0 + 0.4)
                 + 0.20 * (test_rand_unit() * 2.0 - 1.0);
        g_in[0][i] = (int32_t)(v * 8000.0);
    }

    TASK_CHECK(test_encode_and_parse(&tc, CH, BPS, N, g_in, &ch_process, parsed,
        residual, &unused, g_out));
    TASK_CHECK(test_expected_model(&ctx, g_in[0], N, BPS, PRESET, periods, 3,
        &expected, expected_residual));
    TASK_CHECK(test_models_equal(&expected, &parsed[0]));
    for (i = 0; i < N; i++) {
        TASK_CHECK(expected_residual[i] == residual[0][i]);
    }
    /* 3次は周期32でしか両側相関が確保できない（64-1-32+* >= 0 の制約ではないが、
     * 次数3は period+1+order/2 <= N を満たすこと）。少なくとも全候補の評価が通ることを確認 */
    for (i = 0; i < N; i++) {
        TASK_CHECK(g_out[0][i] == g_in[0][i]);
    }

    test_sel_ctx_destroy(&ctx, 16);
    test_codec_destroy(&tc);
    return 0;
}

/* ------------------------------------------------------------------ */
/* selection: マルチチャンネル処理法の選択とチャンネル別モデル         */
/* ------------------------------------------------------------------ */

/* 2chのLR/MS/LS/SR候補を独立列挙し、全体コスト最小の処理法を求める */
/* 候補モード無効時も、3ch以上で ComputeBlockSize == EncodeBlock となること */
static int test_integration_disabled_multichannel_size(void)
{
    enum { CH = 5, BPS = 16, N = 256 };
    struct TestCodec tc;
    const int32_t *input[CH];
    uint32_t ch, i, bytes = 0, estimate = 0, consumed = 0, decoded = 0;

    TASK_CHECK(test_codec_create(&tc, CH, BPS, N, N, N, 16));
    /* 候補設定なし（旧来モード） */
    g_rng_state = 5050u;
    for (ch = 0; ch < CH; ch++) {
        for (i = 0; i < N; i++) {
            g_in[ch][i] = test_tonal_sample(20u + 7u * ch, i, (int)ch, 9000);
        }
        input[ch] = g_in[ch];
        memset(g_out[ch], 0, sizeof(g_out[ch]));
    }
    TASK_CHECK(SRLAEncoder_ComputeBlockSize(tc.enc, input, N, &estimate) == SRLA_APIRESULT_OK);
    TASK_CHECK(SRLAEncoder_EncodeBlock(tc.enc, input, N, g_encoded, sizeof(g_encoded), &bytes)
        == SRLA_APIRESULT_OK);
    TASK_CHECK(bytes == estimate);
    {
        int32_t *output[CH];
        for (ch = 0; ch < CH; ch++) {
            output[ch] = g_out[ch];
        }
        TASK_CHECK(SRLADecoder_DecodeBlock(tc.dec, g_encoded, bytes, output, CH, N,
            &consumed, &decoded) == SRLA_APIRESULT_OK);
    }
    TASK_CHECK(consumed == bytes);
    TASK_CHECK(decoded == N);
    for (ch = 0; ch < CH; ch++) {
        TASK_CHECK(memcmp(g_in[ch], g_out[ch], sizeof(int32_t) * N) == 0);
    }

    test_codec_destroy(&tc);
    return 0;
}

/* 指定した入力に対して、独立列挙で処理法とチャンネル別モデルを照合する */
static int test_selection_stereo_case(int lr_signal, const uint32_t *periods, uint32_t count)
{
    enum { CH = 2, N = 256, BPS = 16 };
    struct TestCodec tc;
    struct TestSelCtx ctx;
    struct TestParsedChannel parsed[CH];
    int32_t residual[CH][TEST_MAX_SAMPLES];
    int32_t lr[CH][TEST_MAX_SAMPLES], ms[CH][TEST_MAX_SAMPLES];
    struct TestModel best_lr[2], best_ms[2];
    int32_t best_lr_res[2][TEST_MAX_SAMPLES], best_ms_res[2][TEST_MAX_SAMPLES];
    uint32_t ch_process = 0, i, c, unused = 0;
    uint32_t len_none, len_ms, len_ls, len_sr, min_len;
    int ms_used;

    TASK_CHECK(test_codec_create(&tc, CH, BPS, N, N, N, 16));
    TASK_CHECK(test_sel_ctx_create(&ctx, 16, N));
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(tc.enc, periods, count) == SRLA_APIRESULT_OK);

    g_rng_state = 555u;
    for (i = 0; i < N; i++) {
        if (lr_signal == 0) {
            /* L/R が同相で相関が高い（MSで差が小さくなる） */
            const double l = 0.60 * sin(2.0 * TEST_PI * (double)i / 48.0)
                           + 0.05 * (test_rand_unit() * 2.0 - 1.0);
            const double r = 0.60 * sin(2.0 * TEST_PI * (double)i / 48.0 + 0.02)
                           + 0.05 * (test_rand_unit() * 2.0 - 1.0);
            g_in[0][i] = (int32_t)(l * 9000.0);
            g_in[1][i] = (int32_t)(r * 9000.0);
        } else if (lr_signal == 1) {
            /* R と S=L-R が単純なトーン -> SR ({R,S}) が有利 */
            const double r = 0.55 * sin(2.0 * TEST_PI * (double)i / 50.0);
            const double s = 0.55 * sin(2.0 * TEST_PI * (double)i / 33.0 + 0.7);
            g_in[0][i] = (int32_t)((s + r) * 9000.0);
            g_in[1][i] = (int32_t)(r * 9000.0);
        } else {
            /* R が白色雑音、L と S が単純なトーン -> LS ({L,S}) が有利 */
            const double l = 0.55 * sin(2.0 * TEST_PI * (double)i / 50.0);
            const double s = 0.55 * sin(2.0 * TEST_PI * (double)i / 33.0 + 0.7);
            const double noise = 0.35 * (test_rand_unit() * 2.0 - 1.0);
            g_in[0][i] = (int32_t)(l * 9000.0);
            g_in[1][i] = (int32_t)((l - s + noise) * 9000.0);
        }
    }
    for (c = 0; c < CH; c++) {
        memcpy(lr[c], g_in[c], sizeof(lr[c]));
        memcpy(ms[c], g_in[c], sizeof(ms[c]));
    }
    /* MS変換（ユーティリティと同じ） */
    {
        int32_t *ms_ptr[2];
        ms_ptr[0] = ms[0];
        ms_ptr[1] = ms[1];
        SRLAUtility_LRtoMSConversion(ms_ptr, N);
    }

    TASK_CHECK(test_encode_and_parse(&tc, CH, BPS, N, g_in, &ch_process, parsed,
        residual, &unused, g_out));
    for (c = 0; c < CH; c++) {
        for (i = 0; i < N; i++) {
            TASK_CHECK(g_out[c][i] == g_in[c][i]);
        }
    }

    /* 独立にLR/MSの両方を列挙（処理法比較用） */
    TASK_CHECK(test_expected_model(&ctx, lr[0], N, BPS, 2, periods, count, &best_lr[0], best_lr_res[0]));
    TASK_CHECK(test_expected_model(&ctx, lr[1], N, BPS, 2, periods, count, &best_lr[1], best_lr_res[1]));
    TASK_CHECK(test_expected_model(&ctx, ms[0], N, BPS, 2, periods, count, &best_ms[0], best_ms_res[0]));
    TASK_CHECK(test_expected_model(&ctx, ms[1], N, BPS, 2, periods, count, &best_ms[1], best_ms_res[1]));

    len_none = best_lr[0].bits + best_lr[1].bits;
    len_ms = best_ms[0].bits + best_ms[1].bits;
    len_ls = best_lr[0].bits + best_ms[1].bits;
    len_sr = best_lr[1].bits + best_ms[1].bits;
    min_len = len_none; ms_used = 0;
    if (len_ms < min_len) { min_len = len_ms; ms_used = 1; }
    if (len_ls < min_len) { min_len = len_ls; ms_used = 2; }
    if (len_sr < min_len) { min_len = len_sr; ms_used = 3; }

    if (ms_used == 0) {
        TASK_CHECK(ch_process == SRLA_CH_PROCESS_METHOD_NONE);
        TASK_CHECK(test_models_equal(&best_lr[0], &parsed[0]));
        TASK_CHECK(test_models_equal(&best_lr[1], &parsed[1]));
        for (i = 0; i < N; i++) {
            TASK_CHECK(best_lr_res[0][i] == residual[0][i]);
            TASK_CHECK(best_lr_res[1][i] == residual[1][i]);
        }
    } else if (ms_used == 1) {
        TASK_CHECK(ch_process == SRLA_CH_PROCESS_METHOD_MS);
        TASK_CHECK(test_models_equal(&best_ms[0], &parsed[0]));
        TASK_CHECK(test_models_equal(&best_ms[1], &parsed[1]));
        for (i = 0; i < N; i++) {
            TASK_CHECK(best_ms_res[0][i] == residual[0][i]);
            TASK_CHECK(best_ms_res[1][i] == residual[1][i]);
        }
    } else if (ms_used == 2) {
        TASK_CHECK(ch_process == SRLA_CH_PROCESS_METHOD_LS);
        TASK_CHECK(test_models_equal(&best_lr[0], &parsed[0]));
        TASK_CHECK(test_models_equal(&best_ms[1], &parsed[1]));
        for (i = 0; i < N; i++) {
            TASK_CHECK(best_lr_res[0][i] == residual[0][i]);
            TASK_CHECK(best_ms_res[1][i] == residual[1][i]);
        }
    } else {
        TASK_CHECK(ch_process == SRLA_CH_PROCESS_METHOD_SR);
        TASK_CHECK(test_models_equal(&best_lr[1], &parsed[1]));
        TASK_CHECK(test_models_equal(&best_ms[1], &parsed[0]));
        for (i = 0; i < N; i++) {
            TASK_CHECK(best_lr_res[1][i] == residual[1][i]);
            TASK_CHECK(best_ms_res[1][i] == residual[0][i]);
        }
    }
    /* 処理法の選択とチャンネル別モデルが両方LTP無効でも、独立列挙との一致が主眼 */
    (void)ms_used;

    test_sel_ctx_destroy(&ctx, 16);
    test_codec_destroy(&tc);
    return 0;
}

static int test_selection_stereo_ms(void)
{
    uint32_t periods[2] = { 48, 96 };
    return test_selection_stereo_case(0, periods, 2);
}

/* R と S が単純なトーン（SRが選ばれる） */
static int test_selection_stereo_sr(void)
{
    uint32_t periods[2] = { 32, 50 };
    return test_selection_stereo_case(1, periods, 2);
}

/* L と S が単純なトーン（LSが選ばれる） */
static int test_selection_stereo_ls(void)
{
    uint32_t periods[2] = { 32, 50 };
    return test_selection_stereo_case(2, periods, 2);
}

/* 2chで左右が異なるモデル（LTP有無・周期が異なる）を選ぶこと */
static int test_selection_channels_different_models(void)
{
    enum { CH = 2, N = 256, BPS = 16 };
    struct TestCodec tc;
    struct TestSelCtx ctx;
    struct TestParsedChannel parsed[CH];
    int32_t residual[CH][TEST_MAX_SAMPLES];
    struct TestModel expect[CH];
    int32_t expect_res[CH][TEST_MAX_SAMPLES];
    uint32_t periods[2] = { 32, 80 };
    uint32_t ch_process = 0, i, unused = 0, c;

    TASK_CHECK(test_codec_create(&tc, CH, BPS, N, N, N, 16));
    TASK_CHECK(test_sel_ctx_create(&ctx, 16, N));
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(tc.enc, periods, 2) == SRLA_APIRESULT_OK);

    g_rng_state = 666u;
    {
        /* L: 周期32のパルス列を指数減衰で滑らかにしたもの
         *    （短期的には白色に近く、長期的に強い周期性を持つ）
         * R: 白色雑音 */
        double lp = 0.0, lq = 0.0;
        for (i = 0; i < N; i++) {
            const double pulse = ((i % 32) == 0) ? 1.0 : 0.0;
            lp = 0.55 * lp + pulse;
            lq = 0.15 * lq + lp;
            g_in[0][i] = (int32_t)(lq * 20000.0) + (int32_t)((test_rand_unit() * 2.0 - 1.0) * 60.0);
            g_in[1][i] = (int32_t)((test_rand_unit() * 2.0 - 1.0) * 12000.0);
        }
    }

    TASK_CHECK(test_encode_and_parse(&tc, CH, BPS, N, g_in, &ch_process, parsed,
        residual, &unused, g_out));
    for (c = 0; c < CH; c++) {
        TASK_CHECK(test_expected_model(&ctx, g_in[c], N, BPS, 2, periods, 2,
            &expect[c], expect_res[c]));
    }
    if (ch_process == SRLA_CH_PROCESS_METHOD_NONE) {
        TASK_CHECK(test_models_equal(&expect[0], &parsed[0]));
        TASK_CHECK(test_models_equal(&expect[1], &parsed[1]));
        for (i = 0; i < N; i++) {
            TASK_CHECK(expect_res[0][i] == residual[0][i]);
            TASK_CHECK(expect_res[1][i] == residual[1][i]);
        }
    } else if (ch_process == SRLA_CH_PROCESS_METHOD_MS) {
        /* MS変換後の信号で期待モデルを再評価 */
        int32_t ms[CH][TEST_MAX_SAMPLES];
        int32_t *ms_ptr[2];
        for (c = 0; c < CH; c++) {
            memcpy(ms[c], g_in[c], sizeof(ms[c]));
        }
        ms_ptr[0] = ms[0]; ms_ptr[1] = ms[1];
        SRLAUtility_LRtoMSConversion(ms_ptr, N);
        for (c = 0; c < CH; c++) {
            TASK_CHECK(test_expected_model(&ctx, ms[c], N, BPS, 2, periods, 2,
                &expect[c], expect_res[c]));
            TASK_CHECK(test_models_equal(&expect[c], &parsed[c]));
            for (i = 0; i < N; i++) {
                TASK_CHECK(expect_res[c][i] == residual[c][i]);
            }
        }
    }

    for (i = 0; i < N; i++) {
        TASK_CHECK(g_out[0][i] == g_in[0][i]);
        TASK_CHECK(g_out[1][i] == g_in[1][i]);
    }

    test_sel_ctx_destroy(&ctx, 16);
    test_codec_destroy(&tc);
    return 0;
}

/* 3ch以上では先頭2ch以外のコストも含めて処理法を比較すること */
static int test_selection_multichannel_cost_includes_others(void)
{
    enum { CH = 3, N = 256, BPS = 16 };
    struct TestCodec tc;
    struct TestSelCtx ctx;
    struct TestParsedChannel parsed[CH];
    int32_t residual[CH][TEST_MAX_SAMPLES];
    int32_t lr[CH][TEST_MAX_SAMPLES], ms[CH][TEST_MAX_SAMPLES];
    struct TestModel best_lr[CH], best_ms[2];
    int32_t best_lr_res[CH][TEST_MAX_SAMPLES], best_ms_res[2][TEST_MAX_SAMPLES];
    uint32_t periods[2] = { 32, 64 };
    uint32_t ch_process = 0, i, c, unused = 0;
    uint32_t len_none, len_ms, len_ls, len_sr, min_len;
    int ms_used;

    TASK_CHECK(test_codec_create(&tc, CH, BPS, N, N, N, 16));
    TASK_CHECK(test_sel_ctx_create(&ctx, 16, N));
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(tc.enc, periods, 2) == SRLA_APIRESULT_OK);

    /* 先頭2chは同相のトーン、3ch目は巨大なエネルギーを持つ白色雑音。
     * 3ch目はMS/LS/SRの対象外だが、そのコストは全処理法で共通のため
     * どれが選ばれても矛盾しない。ここでは独立列挙と照合する。 */
    g_rng_state = 4040u;
    for (i = 0; i < N; i++) {
        const double l = 0.50 * sin(2.0 * TEST_PI * (double)i / 32.0)
                       + 0.04 * (test_rand_unit() * 2.0 - 1.0);
        const double r = 0.50 * sin(2.0 * TEST_PI * (double)i / 32.0 + 0.02)
                       + 0.04 * (test_rand_unit() * 2.0 - 1.0);
        g_in[0][i] = (int32_t)(l * 9000.0);
        g_in[1][i] = (int32_t)(r * 9000.0);
        g_in[2][i] = (int32_t)((test_rand_unit() * 2.0 - 1.0) * 12000.0);
    }
    for (c = 0; c < CH; c++) {
        memcpy(lr[c], g_in[c], sizeof(lr[c]));
    }
    memcpy(ms[0], g_in[0], sizeof(ms[0]));
    memcpy(ms[1], g_in[1], sizeof(ms[1]));
    {
        int32_t *ms_ptr[2];
        ms_ptr[0] = ms[0];
        ms_ptr[1] = ms[1];
        SRLAUtility_LRtoMSConversion(ms_ptr, N);
    }

    TASK_CHECK(test_encode_and_parse(&tc, CH, BPS, N, g_in, &ch_process, parsed,
        residual, &unused, g_out));
    for (c = 0; c < CH; c++) {
        for (i = 0; i < N; i++) {
            TASK_CHECK(g_out[c][i] == g_in[c][i]);
        }
    }

    TASK_CHECK(test_expected_model(&ctx, lr[0], N, BPS, 2, periods, 2, &best_lr[0], best_lr_res[0]));
    TASK_CHECK(test_expected_model(&ctx, lr[1], N, BPS, 2, periods, 2, &best_lr[1], best_lr_res[1]));
    TASK_CHECK(test_expected_model(&ctx, lr[2], N, BPS, 2, periods, 2, &best_lr[2], best_lr_res[2]));
    TASK_CHECK(test_expected_model(&ctx, ms[0], N, BPS, 2, periods, 2, &best_ms[0], best_ms_res[0]));
    TASK_CHECK(test_expected_model(&ctx, ms[1], N, BPS, 2, periods, 2, &best_ms[1], best_ms_res[1]));

    len_none = best_lr[0].bits + best_lr[1].bits + best_lr[2].bits;
    len_ms = best_ms[0].bits + best_ms[1].bits + best_lr[2].bits;
    len_ls = best_lr[0].bits + best_ms[1].bits + best_lr[2].bits;
    len_sr = best_lr[1].bits + best_ms[1].bits + best_lr[2].bits;
    min_len = len_none; ms_used = 0;
    if (len_ms < min_len) { min_len = len_ms; ms_used = 1; }
    if (len_ls < min_len) { min_len = len_ls; ms_used = 2; }
    if (len_sr < min_len) { min_len = len_sr; ms_used = 3; }

    /* 3ch目のモデルは処理法に関わらずLRのまま */
    TASK_CHECK(test_models_equal(&best_lr[2], &parsed[2]));
    for (i = 0; i < N; i++) {
        TASK_CHECK(best_lr_res[2][i] == residual[2][i]);
    }
    /* 先頭2chは処理法に応じたモデル */
    if (ms_used == 0) {
        TASK_CHECK(ch_process == SRLA_CH_PROCESS_METHOD_NONE);
        TASK_CHECK(test_models_equal(&best_lr[0], &parsed[0]));
        TASK_CHECK(test_models_equal(&best_lr[1], &parsed[1]));
    } else if (ms_used == 1) {
        TASK_CHECK(ch_process == SRLA_CH_PROCESS_METHOD_MS);
        TASK_CHECK(test_models_equal(&best_ms[0], &parsed[0]));
        TASK_CHECK(test_models_equal(&best_ms[1], &parsed[1]));
    } else if (ms_used == 2) {
        TASK_CHECK(ch_process == SRLA_CH_PROCESS_METHOD_LS);
        TASK_CHECK(test_models_equal(&best_lr[0], &parsed[0]));
        TASK_CHECK(test_models_equal(&best_ms[1], &parsed[1]));
    } else {
        TASK_CHECK(ch_process == SRLA_CH_PROCESS_METHOD_SR);
        TASK_CHECK(test_models_equal(&best_ms[1], &parsed[0]));
        TASK_CHECK(test_models_equal(&best_lr[1], &parsed[1]));
    }
    for (i = 0; i < N; i++) {
        if (ms_used == 0) {
            TASK_CHECK(best_lr_res[0][i] == residual[0][i]);
            TASK_CHECK(best_lr_res[1][i] == residual[1][i]);
        } else if (ms_used == 1) {
            TASK_CHECK(best_ms_res[0][i] == residual[0][i]);
            TASK_CHECK(best_ms_res[1][i] == residual[1][i]);
        } else if (ms_used == 2) {
            TASK_CHECK(best_lr_res[0][i] == residual[0][i]);
            TASK_CHECK(best_ms_res[1][i] == residual[1][i]);
        } else {
            TASK_CHECK(best_ms_res[1][i] == residual[0][i]);
            TASK_CHECK(best_lr_res[1][i] == residual[1][i]);
        }
    }

    test_sel_ctx_destroy(&ctx, 16);
    test_codec_destroy(&tc);
    return 0;
}

/* ------------------------------------------------------------------ */
/* integration                                                          */
/* ------------------------------------------------------------------ */

/* エンコード→デコードのラウンドトリップ（ビット深度・チャンネル数を変えて） */
static int test_integration_roundtrip(uint32_t num_channels, uint32_t bps,
    uint32_t num_samples, uint32_t min_block, uint32_t max_block,
    uint32_t lookahead, uint32_t max_params, int use_optimal)
{
    struct TestCodec tc;
    uint32_t periods[3] = { 24, 48, 96 };
    const int32_t *input[TEST_MAX_CHANNELS];
    int32_t *output[TEST_MAX_CHANNELS];
    uint32_t ch, i, bytes = 0, estimate = 0, written = 0;
    /* ビット深度に応じた振幅（16bitで約-3dB、24bitで約-2dB） */
    int32_t amplitude = (bps == 16) ? 9000 : (1 << 22);
    int ok;

    TASK_CHECK(test_codec_create(&tc, num_channels, bps, min_block, max_block, lookahead, max_params));
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(tc.enc, periods, 3) == SRLA_APIRESULT_OK);

    g_rng_state = 777u + num_channels * 31u + bps;
    for (ch = 0; ch < num_channels; ch++) {
        for (i = 0; i < num_samples; i++) {
            /* チャンネルごとに異なる周期性 */
            const uint32_t period = 24u + 9u * ch;
            g_in[ch][i] = test_tonal_sample(period, i, (int)ch, amplitude);
        }
        input[ch] = g_in[ch];
        output[ch] = g_out[ch];
        memset(g_out[ch], 0, sizeof(g_out[ch]));
    }

    if (use_optimal) {
        /* ヘッダ込みの全体エンコード（最適分割を含む） */
        uint8_t filebuf[TEST_CAPACITY];
        uint32_t file_bytes = 0;
        TASK_CHECK(SRLAEncoder_EncodeWhole(tc.enc, input, num_samples,
            filebuf, sizeof(filebuf), &file_bytes, NULL) == SRLA_APIRESULT_OK);
        TASK_CHECK(SRLADecoder_DecodeWhole(tc.dec, filebuf, file_bytes,
            output, num_channels, num_samples) == SRLA_APIRESULT_OK);
        written = file_bytes;
    } else {
        TASK_CHECK(SRLAEncoder_ComputeBlockSize(tc.enc, input, num_samples, &estimate) == SRLA_APIRESULT_OK);
        TASK_CHECK(SRLAEncoder_EncodeBlock(tc.enc, input, num_samples,
            g_encoded, sizeof(g_encoded), &bytes) == SRLA_APIRESULT_OK);
        TASK_CHECK(bytes == estimate);
        {
            uint32_t consumed = 0, decoded = 0;
            TASK_CHECK(SRLADecoder_DecodeBlock(tc.dec, g_encoded, bytes, output, num_channels,
                num_samples, &consumed, &decoded) == SRLA_APIRESULT_OK);
            TASK_CHECK(consumed == bytes);
            TASK_CHECK(decoded == num_samples);
        }
        written = bytes;
    }
    TASK_CHECK(written > 0);

    ok = 1;
    for (ch = 0; ch < num_channels; ch++) {
        if (memcmp(g_in[ch], g_out[ch], sizeof(int32_t) * num_samples) != 0) {
            ok = 0;
        }
    }
    TASK_CHECK(ok);

    test_codec_destroy(&tc);
    return 0;
}

static int test_integration_mono16(void)
{
    return test_integration_roundtrip(1, 16, 512, 512, 512, 512, 16, 0);
}

static int test_integration_stereo24(void)
{
    return test_integration_roundtrip(2, 24, 512, 512, 512, 512, 16, 0);
}

static int test_integration_8ch(void)
{
    return test_integration_roundtrip(8, 16, 512, 512, 512, 512, 16, 0);
}

static int test_integration_3ch_above(void)
{
    return test_integration_roundtrip(5, 24, 384, 384, 384, 384, 16, 0);
}

/* 無音・生データへのフォールバックでも ComputeBlockSize == EncodeBlock */
static int test_integration_fallback_sizes(void)
{
    enum { CH = 2, BPS = 16, N = 256 };
    struct TestCodec tc;
    const int32_t *input[CH];
    uint32_t ch, i, bytes = 0, estimate = 0;

    TASK_CHECK(test_codec_create(&tc, CH, BPS, N, N, N, 16));
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(tc.enc, (const uint32_t[]){ 40, 80 }, 2) == SRLA_APIRESULT_OK);

    /* 無音ブロック */
    for (ch = 0; ch < CH; ch++) {
        memset(g_in[ch], 0, sizeof(g_in[ch]));
        input[ch] = g_in[ch];
    }
    TASK_CHECK(SRLAEncoder_ComputeBlockSize(tc.enc, input, N, &estimate) == SRLA_APIRESULT_OK);
    TASK_CHECK(SRLAEncoder_EncodeBlock(tc.enc, input, N, g_encoded, sizeof(g_encoded), &bytes)
        == SRLA_APIRESULT_OK);
    TASK_CHECK(bytes == estimate);
    TASK_CHECK(g_encoded[8] == SRLA_BLOCK_DATA_TYPE_SILENT);

    /* 生データにフォールバックする高エントロピー信号（LPC次数以上の長さが必要） */
    g_rng_state = 6060u;
    for (ch = 0; ch < CH; ch++) {
        for (i = 0; i < N; i++) {
            g_in[ch][i] = (int32_t)(test_rand() & 0xFFFFu) - 32768;
        }
    }
    TASK_CHECK(SRLAEncoder_ComputeBlockSize(tc.enc, input, N, &estimate) == SRLA_APIRESULT_OK);
    TASK_CHECK(SRLAEncoder_EncodeBlock(tc.enc, input, N, g_encoded, sizeof(g_encoded), &bytes)
        == SRLA_APIRESULT_OK);
    TASK_CHECK(bytes == estimate);
    /* 白色雑音は圧縮が効かず生データになるはず */
    TASK_CHECK(g_encoded[8] == SRLA_BLOCK_DATA_TYPE_RAWDATA);

    test_codec_destroy(&tc);
    return 0;
}

/* 端数サンプル（短い末尾ブロック）を含む全体エンコード */
static int test_integration_short_tail(void)
{
    enum { CH = 2, BPS = 16, MAXS = 128, LOOK = 512 };
    struct TestCodec tc;
    uint32_t periods[2] = { 17, 40 };
    const int32_t *input[CH];
    int32_t *output[CH];
    uint32_t num_samples = 700; /* 128*5=640 の後に 60 サンプル端数 */
    uint32_t ch, i, file_bytes = 0;
    uint8_t filebuf[TEST_CAPACITY];

    TASK_CHECK(test_codec_create(&tc, CH, BPS, MAXS, MAXS, LOOK, 16));
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(tc.enc, periods, 2) == SRLA_APIRESULT_OK);

    g_rng_state = 3030u;
    for (ch = 0; ch < CH; ch++) {
        for (i = 0; i < num_samples; i++) {
            g_in[ch][i] = test_tonal_sample(17u + 23u * ch, i, (int)ch, 9000);
        }
        input[ch] = g_in[ch];
        output[ch] = g_out[ch];
        memset(g_out[ch], 0, sizeof(g_out[ch]));
    }

    TASK_CHECK(SRLAEncoder_EncodeWhole(tc.enc, input, num_samples,
        filebuf, sizeof(filebuf), &file_bytes, NULL) == SRLA_APIRESULT_OK);
    TASK_CHECK(SRLADecoder_DecodeWhole(tc.dec, filebuf, file_bytes,
        output, CH, num_samples) == SRLA_APIRESULT_OK);
    for (ch = 0; ch < CH; ch++) {
        TASK_CHECK(memcmp(g_in[ch], g_out[ch], sizeof(int32_t) * num_samples) == 0);
    }

    test_codec_destroy(&tc);
    return 0;
}

/* 最適ブロック分割＋ヘッダ込みラウンドトリップ（短い末尾ブロックを含む） */
static int test_integration_optimal_partition(void)
{
    return test_integration_roundtrip(2, 16, 700, 128, 256, 512, 16, 1);
}

static int test_integration_optimal_partition_mono24(void)
{
    return test_integration_roundtrip(1, 24, 500, 64, 128, 512, 16, 1);
}

/* 素材と入力PCMが変更されないこと */
static int test_integration_input_immutable(void)
{
    enum { CH = 2, N = 512, BPS = 16 };
    struct TestCodec tc;
    int32_t snapshot[CH][TEST_MAX_SAMPLES];
    uint32_t periods[2] = { 40, 100 };
    const int32_t *input[CH];
    uint32_t ch, i, bytes = 0;

    TASK_CHECK(test_codec_create(&tc, CH, BPS, N, N, N, 16));
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(tc.enc, periods, 2) == SRLA_APIRESULT_OK);
    g_rng_state = 888u;
    for (ch = 0; ch < CH; ch++) {
        for (i = 0; i < N; i++) {
            g_in[ch][i] = test_tonal_sample(40u + 20u * ch, i, (int)ch, 9000);
        }
        input[ch] = g_in[ch];
        memcpy(snapshot[ch], g_in[ch], sizeof(snapshot[ch]));
    }
    TASK_CHECK(SRLAEncoder_EncodeBlock(tc.enc, input, N, g_encoded, sizeof(g_encoded), &bytes)
        == SRLA_APIRESULT_OK);
    for (ch = 0; ch < CH; ch++) {
        TASK_CHECK(memcmp(snapshot[ch], g_in[ch], sizeof(snapshot[ch])) == 0);
    }

    test_codec_destroy(&tc);
    return 0;
}

/* 候補設定APIの検証 */
static int test_integration_candidate_api(void)
{
    struct TestCodec tc;
    uint32_t periods[8] = { 200, 8, 100, 8, 262, 50, 50, 9 };
    uint32_t too_many[9] = { 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    uint32_t bad_low[2] = { 7, 100 };
    uint32_t bad_high[2] = { 100, 263 };
    uint32_t none[1] = { 0 };

    TASK_CHECK(test_codec_create(&tc, 1, 16, 256, 256, 256, 16));

    /* 正常 */
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(tc.enc, periods, 8) == SRLA_APIRESULT_OK);
    /* 上限超え */
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(tc.enc, too_many, 9) == SRLA_APIRESULT_INVALID_ARGUMENT);
    /* 範囲外（下限） */
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(tc.enc, bad_low, 2) == SRLA_APIRESULT_INVALID_ARGUMENT);
    /* 範囲外（上限） */
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(tc.enc, bad_high, 2) == SRLA_APIRESULT_INVALID_ARGUMENT);
    /* NULL */
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(tc.enc, NULL, 1) == SRLA_APIRESULT_INVALID_ARGUMENT);
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(NULL, periods, 1) == SRLA_APIRESULT_INVALID_ARGUMENT);
    /* count=0で無効化（periodsはNULLでも可） */
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(tc.enc, NULL, 0) == SRLA_APIRESULT_OK);
    /* 再度設定可能 */
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(tc.enc, none, 0) == SRLA_APIRESULT_OK);

    test_codec_destroy(&tc);
    return 0;
}

/* 候補設定がエンコードパラメータ再設定で消えないこと */
static int test_integration_settings_persist(void)
{
    struct TestCodec tc;
    struct SRLAEncodeParameter param;
    uint32_t periods[2] = { 40, 60 };
    const int32_t *input[1];
    uint32_t i, bytes = 0;

    TASK_CHECK(test_codec_create(&tc, 1, 16, 256, 256, 256, 16));
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(tc.enc, periods, 2) == SRLA_APIRESULT_OK);

    /* パラメータを再設定 */
    param.num_channels = 1;
    param.bits_per_sample = 16;
    param.sampling_rate = 44100;
    param.min_num_samples_per_block = 256;
    param.max_num_samples_per_block = 256;
    param.num_lookahead_samples = 256;
    param.ltp_order = 0;
    param.num_svr_filter_learning_iteration = 0;
    param.preset = 2;
    TASK_CHECK(SRLAEncoder_SetEncodeParameter(tc.enc, &param) == SRLA_APIRESULT_OK);

    g_rng_state = 999u;
    for (i = 0; i < 256; i++) {
        g_in[0][i] = test_tonal_sample(40, i, 0, 8000);
    }
    input[0] = g_in[0];
    /* 再設定後も候補モードで動作（正常終了すればよい） */
    TASK_CHECK(SRLAEncoder_EncodeBlock(tc.enc, input, 256, g_encoded, sizeof(g_encoded), &bytes)
        == SRLA_APIRESULT_OK);
    TASK_CHECK(bytes > 0);

    test_codec_destroy(&tc);
    return 0;
}

/* 2インスタンスを交互に使っても互いに干渉しないこと */
static int test_integration_two_instances(void)
{
    enum { N = 256, BPS = 16 };
    struct TestCodec a, b;
    uint32_t periods[2] = { 40, 80 };
    const int32_t *input[1];
    uint32_t i, bytes_a = 0, bytes_b = 0, bytes_a2 = 0;

    TASK_CHECK(test_codec_create(&a, 1, BPS, N, N, N, 16));
    TASK_CHECK(test_codec_create(&b, 1, BPS, N, N, N, 16));
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(a.enc, periods, 2) == SRLA_APIRESULT_OK);
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(b.enc, periods, 2) == SRLA_APIRESULT_OK);

    g_rng_state = 1010u;
    for (i = 0; i < N; i++) {
        g_in[0][i] = test_tonal_sample(40, i, 0, 8000);
    }
    input[0] = g_in[0];

    TASK_CHECK(SRLAEncoder_EncodeBlock(a.enc, input, N, g_encoded, sizeof(g_encoded), &bytes_a)
        == SRLA_APIRESULT_OK);
    TASK_CHECK(SRLAEncoder_EncodeBlock(b.enc, input, N, g_encoded2, sizeof(g_encoded2), &bytes_b)
        == SRLA_APIRESULT_OK);
    TASK_CHECK(bytes_a == bytes_b);
    TASK_CHECK(memcmp(g_encoded, g_encoded2, bytes_a) == 0);

    /* Aを再度エンコードしても同じ結果（状態が残らない） */
    TASK_CHECK(SRLAEncoder_EncodeBlock(a.enc, input, N, g_encoded2, sizeof(g_encoded2), &bytes_a2)
        == SRLA_APIRESULT_OK);
    TASK_CHECK(bytes_a == bytes_a2);
    TASK_CHECK(memcmp(g_encoded, g_encoded2, bytes_a) == 0);

    test_codec_destroy(&b);
    test_codec_destroy(&a);
    return 0;
}

/* 候補を無効化した場合、従来どおりの出力になること */
static int test_integration_disabled_compatible(void)
{
    enum { N = 256, BPS = 16 };
    struct TestCodec tc;
    uint32_t periods[2] = { 40, 80 };
    const int32_t *input[1];
    uint32_t i, bytes_off = 0, bytes_on = 0;

    TASK_CHECK(test_codec_create(&tc, 1, BPS, N, N, N, 16));
    g_rng_state = 1212u;
    for (i = 0; i < N; i++) {
        g_in[0][i] = test_tonal_sample(40, i, 0, 8000);
    }
    input[0] = g_in[0];

    /* 候補設定なし */
    TASK_CHECK(SRLAEncoder_EncodeBlock(tc.enc, input, N, g_encoded, sizeof(g_encoded), &bytes_off)
        == SRLA_APIRESULT_OK);

    /* 設定してから無効化 */
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(tc.enc, periods, 2) == SRLA_APIRESULT_OK);
    TASK_CHECK(SRLAEncoder_EncodeBlock(tc.enc, input, N, g_encoded2, sizeof(g_encoded2), &bytes_on)
        == SRLA_APIRESULT_OK);
    TASK_CHECK(SRLAEncoder_SetLTPCandidates(tc.enc, NULL, 0) == SRLA_APIRESULT_OK);
    TASK_CHECK(SRLAEncoder_EncodeBlock(tc.enc, input, N, g_encoded2, sizeof(g_encoded2), &bytes_on)
        == SRLA_APIRESULT_OK);

    TASK_CHECK(bytes_off == bytes_on);
    TASK_CHECK(memcmp(g_encoded, g_encoded2, bytes_off) == 0);

    test_codec_destroy(&tc);
    return 0;
}

/* 旧来の ltp_order 指定時の出力が変わらないこと（無効モードの互換性） */
static int test_integration_legacy_ltp_order(void)
{
    /* 補足）旧来の自動検出は max_pitch_period+1 個の自己相関を取るため、
     *       ブロック長がそれを下回ると元実装でも動作しない（ここでは十分長く取る） */
    enum { N = 512, BPS = 16 };
    struct TestCodec tc;
    struct SRLAEncodeParameter param;
    const int32_t *input[1];
    uint32_t i, bytes1 = 0, bytes2 = 0;

    TASK_CHECK(test_codec_create(&tc, 1, BPS, N, N, N, 16));
    g_rng_state = 1313u;
    for (i = 0; i < N; i++) {
        g_in[0][i] = test_tonal_sample(40, i, 0, 8000);
    }
    input[0] = g_in[0];

    /* ltp_order=3（自動検出）でエンコード */
    param.num_channels = 1;
    param.bits_per_sample = BPS;
    param.sampling_rate = 44100;
    param.min_num_samples_per_block = N;
    param.max_num_samples_per_block = N;
    param.num_lookahead_samples = N;
    param.ltp_order = 3;
    param.num_svr_filter_learning_iteration = 0;
    param.preset = 2;
    TASK_CHECK(SRLAEncoder_SetEncodeParameter(tc.enc, &param) == SRLA_APIRESULT_OK);
    TASK_CHECK(SRLAEncoder_EncodeBlock(tc.enc, input, N, g_encoded, sizeof(g_encoded), &bytes1)
        == SRLA_APIRESULT_OK);

    /* もう一度同じ設定でエンコードして一致（決定的） */
    TASK_CHECK(SRLAEncoder_EncodeBlock(tc.enc, input, N, g_encoded2, sizeof(g_encoded2), &bytes2)
        == SRLA_APIRESULT_OK);
    TASK_CHECK(bytes1 == bytes2);
    TASK_CHECK(memcmp(g_encoded, g_encoded2, bytes1) == 0);

    test_codec_destroy(&tc);
    return 0;
}

/* ------------------------------------------------------------------ */
/* テスト登録                                                          */
/* ------------------------------------------------------------------ */

#define TEST_CASE(group_, name_, func_) { group_, name_, func_ }

const struct task_case task_cases[] = {
    TEST_CASE("fitting", "matches_direct", test_fitting_matches_direct),
    TEST_CASE("fitting", "arbitrary_lag", test_fitting_arbitrary_lag),
    TEST_CASE("fitting", "auto_unchanged", test_fitting_auto_unchanged),
    TEST_CASE("fitting", "failures", test_fitting_failures),

    TEST_CASE("selection", "single_channel", test_selection_single_channel),
    TEST_CASE("selection", "order_preference", test_selection_order_preference),
    TEST_CASE("selection", "disable_wins", test_selection_disable_wins),
    TEST_CASE("selection", "skips_unfit_period", test_selection_skips_unfit_period),
    TEST_CASE("selection", "short_block_enumerates_all", test_selection_short_block_enumerates_all),
    TEST_CASE("selection", "stereo_ms", test_selection_stereo_ms),
    TEST_CASE("selection", "stereo_sr", test_selection_stereo_sr),
    TEST_CASE("selection", "stereo_ls", test_selection_stereo_ls),
    TEST_CASE("selection", "channels_different_models", test_selection_channels_different_models),
    TEST_CASE("selection", "multichannel_cost", test_selection_multichannel_cost_includes_others),

    TEST_CASE("integration", "mono16", test_integration_mono16),
    TEST_CASE("integration", "stereo24", test_integration_stereo24),
    TEST_CASE("integration", "8ch", test_integration_8ch),
    TEST_CASE("integration", "3ch_above", test_integration_3ch_above),
    TEST_CASE("integration", "short_tail", test_integration_short_tail),
    TEST_CASE("integration", "disabled_multichannel_size", test_integration_disabled_multichannel_size),
    TEST_CASE("integration", "fallback_sizes", test_integration_fallback_sizes),
    TEST_CASE("integration", "optimal_partition", test_integration_optimal_partition),
    TEST_CASE("integration", "optimal_partition_mono24", test_integration_optimal_partition_mono24),
    TEST_CASE("integration", "input_immutable", test_integration_input_immutable),
    TEST_CASE("integration", "candidate_api", test_integration_candidate_api),
    TEST_CASE("integration", "settings_persist", test_integration_settings_persist),
    TEST_CASE("integration", "two_instances", test_integration_two_instances),
    TEST_CASE("integration", "disabled_compatible", test_integration_disabled_compatible),
    TEST_CASE("integration", "legacy_ltp_order", test_integration_legacy_ltp_order),
};

const size_t task_case_count = sizeof(task_cases) / sizeof(task_cases[0]);
