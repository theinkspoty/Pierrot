#include "FrameCache.h"

std::atomic<quint64> FrameCache::s_hits{0};
std::atomic<quint64> FrameCache::s_misses{0};

FrameCache& FrameCache::instance() {
    static FrameCache c;
    return c;
}

QImage FrameCache::get(const Key& key) {
    QMutexLocker l(&m_mutex);
    auto it = m_idx.find(key);
    if (it == m_idx.end()) {
        ++s_misses;
        return QImage();
    }
    ++s_hits;
    // Move-to-front O(1): splice move o nó para a frente sem realocar e sem
    // invalidar o iterador guardado no índice.
    m_lru.splice(m_lru.begin(), m_lru, it.value());
    return it.value()->img;
}

void FrameCache::put(const Key& key, const QImage& img) {
    if (img.isNull()) return;
    QMutexLocker l(&m_mutex);
    auto it = m_idx.find(key);
    if (it != m_idx.end()) {
        it.value()->img = img;
        m_lru.splice(m_lru.begin(), m_lru, it.value());
        return;
    }
    m_lru.push_front({key, img});
    m_idx.insert(key, m_lru.begin());
    while ((int)m_lru.size() > m_max) {
        m_idx.remove(m_lru.back().key);
        m_lru.pop_back();
    }
}

QImage FrameCache::getOrCompute(const Key& key, const std::function<QImage()>& compute) {
    QImage img = get(key);
    if (!img.isNull()) return img;
    img = compute();
    if (!img.isNull()) put(key, img);
    return img;
}

void FrameCache::clear() {
    QMutexLocker l(&m_mutex);
    m_lru.clear();
    m_idx.clear();
}

int FrameCache::size() const {
    QMutexLocker l(&m_mutex);
    return m_lru.size();
}
