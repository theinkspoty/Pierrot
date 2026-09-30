// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#pragma once

#include <QString>
#include <QStringList>
#include <QThread>

// Instala handlers de sinais fatais (SIGSEGV, SIGABRT, etc.) que geram um
// relatório de crash com backtrace e informações do sistema, salvos em um
// arquivo. Deve ser chamado cedo no main().
//
// O relatório inclui a thread que quebrou (backtrace via execinfo) e um
// inventário das demais threads: as registradas por registerThread() com o
// que estavam fazendo, e todas as threads do processo lidas de
// /proc/self/task — inclusive as que ninguém registrou (pool do Qt, threads
// internas do FFmpeg). É o que permite localizar um SIGSEGV em decode
// concorrente, onde a thread culpada raramente é a que quebrou.
namespace CrashReporter {

// Instala os handlers de sinal. Idempotente.
void install();

// Caminho do próximo arquivo de relatório de crash.
QString nextReportPath();
// Lista os arquivos de relatório de crash existentes (para avisar o usuário).
QStringList existingReports();

// Registra a thread corrente sob um nome legível, para o relatório de crash.
// Idempotente por thread (re-registrar troca o nome). `name` e os valores
// passados a setActivity() devem ser literais com vida estática — o relatório
// é escrito de dentro de um handler de sinal e não pode alocar.
void registerThread(const char* name);

// Libera o registro da thread corrente.
void unregisterThread();

// Marca o que a thread corrente está fazendo ("decodificando quadro em
// background", "conformando áudio", ...). Passa nullptr para limpar.
// Barato e sem alocação: pode ser chamado em laço apertado.
void setActivity(const char* activity);

// QThread que se registra sozinho no relatório de crash enquanto roda. É o
// jeito barato de instrumentar os workers: onde um SIGSEGV no decode
// concorrente acontece, o relatório passa a dizer qual thread estava
// decodificando o quê no instante da queda.
//
// O registro acontece num DirectConnection para QThread::started, e NÃO em
// run(). O Qt emite `started` ANTES de chamar run(), então um worker cujo slot
// está ligado a `started` e bloqueia ali (o padrão de exportação, que roda o
// build até o fim) nunca deixaria run() ser chamado — e a thread mais
// importante seria justamente a que faltaria no relatório. A conexão é feita
// no construtor para exploitation tar antes de qualquer connect() do worker:
// o Qt invoca slots na ordem em que foram conectados.
class TrackedThread : public QThread {
public:
    // `name` deve ser literal estático (fica no relatório).
    explicit TrackedThread(const char* name, QObject* parent = nullptr)
        : QThread(parent), m_name(name) {
        connect(this, &QThread::started, this, [this] { registerThread(m_name); },
                Qt::DirectConnection);
        connect(this, &QThread::finished, this, [] { unregisterThread(); },
                Qt::DirectConnection);
    }

    // Sobrescrever run() é permitido: o registro não depende mais dele.
    using QThread::run;

private:
    const char* m_name;
};

} // namespace CrashReporter
