#include "FrameCache.h"

#include <cstdlib>

std::atomic<quint64> FrameCache::s_hits{0};
std::atomic<quint64> FrameCache::s_misses{0};

FrameCache& FrameCache::instance() {
    // Budget de memória do cache global: padrão 512 MB, ajustável por
    // PIERROT_FRAMECACHE_MB (mesmo idioma do PIERROT_DECODER_POOL). Mantém o
    // teto de contagem (120) como salvaguarda barata.
    static FrameCache c = [] {
        qint64 mb = 512;
        if (const char* e = std::getenv("PIERROT_FRAMECACHE_MB")) {
            const long v = std::strtol(e, nullptr, 10);
            if (v >= 1) mb = v;
        }
        return FrameCache(120, mb * 1024 * 1024);
    }();
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
        m_bytes += img.sizeInBytes() - it.value()->img.sizeInBytes();
        it.value()->img = img;
        m_lru.splice(m_lru.begin(), m_lru, it.value());
        evictLocked();
        return;
    }
    m_lru.push_front({key, img});
    m_idx.insert(key, m_lru.begin());
    m_bytes += img.sizeInBytes();
    evictLocked();
}

// Evicção LRU enquanto QUALQUER limite for excedido. Com ambos os tetos, o
// cache não estoura nem em contagem (fragmentação de muitos quadros pequenos)
// nem em bytes (poucos quadros gigantes).
void FrameCache::evictLocked() {
    while ((int)m_lru.size() > m_max
           || (m_maxBytes > 0 && m_bytes > m_maxBytes)) {
        m_bytes -= m_lru.back().img.sizeInBytes();
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
    m_bytes = 0;
}

int FrameCache::size() const {
    QMutexLocker l(&m_mutex);
    return m_lru.size();
}

qint64 FrameCache::bytes() const {
    QMutexLocker l(&m_mutex);
    return m_bytes;
}
