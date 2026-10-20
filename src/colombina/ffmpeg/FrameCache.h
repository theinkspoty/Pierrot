#pragma once

// FrameCache — L4 (Pierrot). Cache LRU de quadros já escalados/conversão RGBA.
//
// Saiu do núcleo (FFmpegDecoder) na Fase 6: o motor não promete cache, isso é
// política de L4. A chave é (bucket de tempo, maxWidth) — o bucket agrupa todos
// os pedidos do mesmo quadro, de modo que scrub/tick repetidos acertam o LRU em
// vez de re-decodificar.
//
// Não depende de FFmpeg: só Qt (QImage/QHash/QMutex). O núcleo expõe apenas o
// índice de tempo (FFmpegDecoder::frameCacheIndex); os chamadores de L4 montam
// a chave, consultam e armazenam aqui.

#include <QHash>
#include <QImage>
#include <QMutex>
#include <QString>

#include <atomic>
#include <functional>
#include <list>

class FrameCache {
public:
    // time_bucket = floor(seconds * fps) para CFR, ou grade de ms para VFR.
    // source = id da fonte (ex.: qHash do caminho) — a chave PRECISA incluí-lo:
    // o cache é global (compartilhado por arquivos/clipes), então sem a fonte
    // um pedido do arquivo B no mesmo (t, largura) devolveria o quadro de A.
    struct Key {
        quint64 source = 0;
        int64_t bucket = 0;
        int maxW = 0;
        bool operator==(const Key& o) const {
            return source == o.source && bucket == o.bucket && maxW == o.maxW;
        }
        friend inline uint qHash(const Key& k, uint seed = 0) {
            return qHash(k.source, seed)
                 ^ qHash(k.bucket, seed ^ 0x9747b28c)
                 ^ qHash(k.maxW, seed ^ 0x165667b1);
        }
    };

    // maxEntries limita a CONTAGEM; maxBytes (0 = sem limite) limita a
    // MEMÓRIA. Ambos são necessários: 120 quadros são ~60MB em SD mas ~4GB em
    // 4K. O budget em MB é o teto que importa em produção (§8.4).
    explicit FrameCache(int maxEntries = 120, qint64 maxBytes = 0)
        : m_max(maxEntries < 1 ? 1 : maxEntries),
          m_maxBytes(maxBytes < 0 ? 0 : maxBytes) {}

    // Instância compartilhada do processo (padrão L4, como SourcePool::instance).
    // O preview a usa como cache único; testes podem instanciar a sua própria.
    static FrameCache& instance();

    // Devolve o quadro em cache (move-to-front) ou QImage nula em miss.
    // Thread-safe: serializa internamente com m_mutex.
    QImage get(const Key& key);

    // Insere/atualiza o quadro (move-to-front; evicção LRU se cheio).
    void put(const Key& key, const QImage& img);

    // Acerta o cache ou chama compute() e guarda o resultado não nulo. É o
    // idioma do caminho quente de L4: evita repetir lookup/put em cada caller.
    QImage getOrCompute(const Key& key, const std::function<QImage()>& compute);

    void clear();

    int size() const;

    // Bytes atualmente em cache (soma de QImage::sizeInBytes). Diagnóstico do
    // budget de memória.
    qint64 bytes() const;

    // Diagnóstico global — quantas vezes o LRU evitou um decode. Mantido
    // estático (como no motor antigo) para o overlay/testes lerem sem depender
    // da instância; permitiria também somar vários caches.
    static std::atomic<quint64> s_hits, s_misses;
    static void resetStats() { s_hits.store(0); s_misses.store(0); }
    static quint64 hits()   { return s_hits.load(); }
    static quint64 misses() { return s_misses.load(); }

private:
    // Evicção LRU (deve ser chamada com m_mutex preso).
    void evictLocked();

    struct Entry {
        Key key;
        QImage img;
    };
    using List = std::list<Entry>;
    // LRU intrusivo O(1): lista + índice para o ITERADOR. Move-to-front via
    // splice, evicção via pop_back — sem realocar nem invalidar iteradores.
    List m_lru;                                  // frente = mais recente
    QHash<Key, List::iterator> m_idx;
    int m_max;
    qint64 m_maxBytes;                           // 0 = sem limite
    qint64 m_bytes = 0;                          // soma de sizeInBytes
    mutable QMutex m_mutex;
};
