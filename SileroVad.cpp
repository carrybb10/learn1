#include "SileroVad.h"

#include "whisper.h"

#include <QDebug>
#include <QFileInfo>
#include <QThread>

SileroVad::SileroVad() = default;

SileroVad::~SileroVad()
{
    unload();
}

bool SileroVad::load(const QString &modelPath)
{
    unload();

    if (modelPath.isEmpty() || !QFileInfo::exists(modelPath)) {
        qWarning() << "[SileroVad] model not found:" << modelPath;
        return false;
    }

    whisper_vad_context_params params = whisper_vad_default_context_params();
    params.n_threads = qMax(1, QThread::idealThreadCount() / 2);
    params.use_gpu = false;

    m_ctx = whisper_vad_init_from_file_with_params(modelPath.toUtf8().constData(), params);
    if (!m_ctx) {
        qWarning() << "[SileroVad] failed to init model:" << modelPath;
        return false;
    }

    qInfo() << "[SileroVad] loaded, window =" << kWindowSamples << "samples";
    return true;
}

void SileroVad::unload()
{
    if (m_ctx) {
        whisper_vad_free(m_ctx);
        m_ctx = nullptr;
    }
}

bool SileroVad::detect(const float *samples, int count, QVector<float> *probs)
{
    if (!m_ctx || !samples || count <= 0)
        return false;

    if (!whisper_vad_detect_speech_no_reset(m_ctx, samples, count))
        return false;

    const int n = whisper_vad_n_probs(m_ctx);
    const float *p = whisper_vad_probs(m_ctx);
    if (n <= 0 || !p)
        return false;

    if (probs) {
        probs->reserve(probs->size() + n);
        for (int i = 0; i < n; ++i)
            probs->append(p[i]);
    }
    return true;
}

bool SileroVad::detectWhole(const float *samples, int count, QVector<float> *probs)
{
    if (!m_ctx || !samples || count <= 0)
        return false;

    if (!whisper_vad_detect_speech(m_ctx, samples, count))
        return false;

    const int n = whisper_vad_n_probs(m_ctx);
    const float *p = whisper_vad_probs(m_ctx);
    if (n <= 0 || !p)
        return false;

    if (probs) {
        probs->resize(n);
        for (int i = 0; i < n; ++i)
            (*probs)[i] = p[i];
    }
    return true;
}

void SileroVad::resetState()
{
    if (m_ctx)
        whisper_vad_reset_state(m_ctx);
}
