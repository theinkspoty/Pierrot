// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

extern "C" {
#include <libavcodec/avcodec.h>
}

// L1 — ErrorConceal: ocultação de erro (último quadro bom).
//
// Quando um pacote chega corrompido, o decode de um quadro falha, ou o stream
// termina no meio de um GOP, o motor NÃO deve devolver "imagem vazia" (o
// consumidor não distingue isso de "erro" ou de "acabou"). Em vez disso,
// entrega o último quadro válido decodificado e sinaliza o estado.
//
// Este componente é deliberadamente puro (sem I/O, sem estado global):
//   • classifica o resultado de send/receive de pacote do libavcodec;
//   • mantém a referência do último quadro bom;
//   • diz se, no estado atual, cabe ocultar (devolver o último bom) ou deixar
//     o erro transparecer (fim de arquivo limpo, sem quadro bom ainda, etc.).

// Status do resultado de um pedido de quadro. "Falha é tipo, não silêncio"
// (invariante 3): distingue aguarde de erro de acabou de cancelado.
enum class FrameStatus {
    Ok,        // quadro decodificado neste pedido
    Concealed, // erro/mid-GOP: devolvido o último quadro bom
    Pending,   // precisa de mais um pacote (EAGAIN)
    Eof,       // fim do arquivo, sem novo quadro
    Error,     // erro de decode sem quadro bom para ocultar
    Cancelled  // pedido abortado por cancelamento
};

inline const char* frameStatusName(FrameStatus s) {
    switch (s) {
    case FrameStatus::Ok: return "Ok";
    case FrameStatus::Concealed: return "Concealed";
    case FrameStatus::Pending: return "Pending";
    case FrameStatus::Eof: return "Eof";
    case FrameStatus::Error: return "Error";
    case FrameStatus::Cancelled: return "Cancelled";
    }
    return "?";
}

class ErrorConceal {
public:
    ErrorConceal() = default;
    ErrorConceal(const ErrorConceal&) = delete;
    ErrorConceal& operator=(const ErrorConceal&) = delete;
    ~ErrorConceal() { clear(); }

    // Guarda uma REFERÊNCIA do quadro bom (av_frame_ref: o dono pode continuar
    // a usar/liberar o original). Substitui o quadro bom anterior.
    void rememberGood(const AVFrame* fr) {
        if (!fr) return;
        if (!m_good) m_good = av_frame_alloc();
        if (!m_good) return;
        av_frame_unref(m_good);
        av_frame_ref(m_good, fr);
        m_hasGood = true;
    }

    bool hasGood() const { return m_hasGood && m_good != nullptr; }
    AVFrame* lastGood() const { return m_good; }

    // Classifica o retorno de avcodec_send_packet/avcodec_receive_frame.
    //  - ret < 0 e EAGAIN/ERR: precisa de mais pacote / erro.
    // Contexto: decidir entre "Pending" e "Error".
    static bool isAgain(int ret) { return ret == AVERROR(EAGAIN); }
    static bool isEof(int ret) { return ret == AVERROR_EOF; }

    // Regra de decisão: dado o desfecho de um pedido (achou quadro? erro? fim?),
    // decide o status e, se cabível, aponta para o último quadro bom.
    //  - decodedThisRequest: um novo quadro foi produzido (Ok);
    //  - err: houve erro de decode/leitura;
    //  - eof: o stream acabou;
    //  - cancelled: o pedido foi abortado.
    // Preenche `out` com o quadro a exibir (pode ser o último bom) e devolve o
    // status. `out` NIL significa "nada a exibir neste estado".
    FrameStatus decide(bool decodedThisRequest, bool err, bool eof, bool cancelled,
                       AVFrame** out) const {
        if (cancelled) {
            if (out) *out = nullptr;
            return FrameStatus::Cancelled;
        }
        if (decodedThisRequest) {
            if (out) *out = nullptr; // o chamador já tem o quadro recém-decodificado
            return FrameStatus::Ok;
        }
        if ((err || eof) && hasGood()) {
            // Mid-GOP / erro: entrega o último quadro bom.
            if (out) *out = m_good;
            return FrameStatus::Concealed;
        }
        if (err) {
            if (out) *out = nullptr;
            return FrameStatus::Error;
        }
        if (eof) {
            if (out) *out = nullptr;
            return FrameStatus::Eof;
        }
        if (out) *out = nullptr;
        return FrameStatus::Pending;
    }

    void clear() {
        if (m_good) {
            av_frame_unref(m_good);
            av_frame_free(&m_good);
        }
        m_hasGood = false;
    }

private:
    AVFrame* m_good = nullptr;
    bool m_hasGood = false;
};
