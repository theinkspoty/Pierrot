// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "BlenderBridge.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace mesh {

bool blenderAvailable(QString* resolvedPath) {
    // Variáveis de ambiente têm precedência sobre o PATH: quem usa Blender
    // portable ou um build em /opt precisa de um jeito de apontar pra ele.
    const QByteArray env = qgetenv("PIERROT_BLENDER");
    if (!env.isEmpty()) {
        const QString p = QFileInfo(QString::fromLocal8Bit(env)).absoluteFilePath();
        if (QFileInfo::exists(p)) {
            if (resolvedPath) *resolvedPath = p;
            return true;
        }
    }
    const QString exe = QStandardPaths::findExecutable(QStringLiteral("blender"));
    if (exe.isEmpty()) {
        if (resolvedPath) resolvedPath->clear();
        return false;
    }
    if (resolvedPath) *resolvedPath = exe;
    return true;
}

QString blendToObj(const QString& blendPath, QString* error, int timeoutMs,
                   const QString& outDir) {
    const auto fail = [&](const QString& msg) {
        if (error) *error = msg;
        return QString();
    };
    if (error) error->clear();

    if (!QFileInfo::exists(blendPath))
        return fail(QStringLiteral("Arquivo não existe:\n%1").arg(blendPath));

    QString blender;
    if (!blenderAvailable(&blender))
        return fail(QStringLiteral(
            "Blender não encontrado no PATH.\n"
            "Instale o Blender (https://www.blender.org) ou exporte "
            ".obj/.glb manualmente no Blender."));

    QTemporaryDir tmp;
    if (!tmp.isValid())
        return fail(QStringLiteral("Não criou diretório temporário."));
    const QString rawObj = tmp.filePath(QStringLiteral("pierrot_export.obj"));

    // O script roda duas vezes: o exportador novo (wm.obj_export, Blender 3.3+)
    // e o antigo (export_scene.obj). Os keywords são passados só no novo —
    // se uma versão não os aceitar, o TypeError cai no except e o fallback
    // antigo roda sem kwargs, que é aceito em todas as versões.
    //
    // use_selection=False é explícito de propósito: o Blender 4.x entrega com
    // nada selecionado ao abrir o .blend, e sem isso a cena inteira sairia
    // vazia. export_materials=True traz o .mtl e as texturas junto, que é o
    // que o ObjLoader procura do lado do .obj.
    const QString pyModern = QStringLiteral(
        "import bpy, sys\n"
        "out = sys.argv[-1]\n"
        "try:\n"
        "    bpy.ops.wm.obj_export(filepath=out, use_selection=False,\n"
        "                        export_materials=True, apply_modifiers=True)\n"
        "except Exception:\n"
        "    bpy.ops.export_scene.obj(filepath=out)\n");
    const QString pyOld = QStringLiteral(
        "import bpy, sys\n"
        "bpy.ops.export_scene.obj(filepath=sys.argv[-1])\n");

    QString log;
    auto runBlender = [&](const QString& py) -> bool {
        QProcess proc;
        proc.setProgram(blender);
        // blender -b file.blend --python-expr "..." -- /tmp/out.obj
        // O último argv é o path do .obj (sys.argv[-1] no script).
        QStringList args;
        args << QStringLiteral("-b") << blendPath
             << QStringLiteral("--python-expr") << py
             << QStringLiteral("--") << rawObj;
        proc.setArguments(args);
        proc.setProcessChannelMode(QProcess::MergedChannels);

        // Drenar durante a execução é obrigatório: sem consumer, o Blender
        // bloqueia ao encher o pipe e só morre no timeout. Ler no fim não
        // adianta — o deadlock acontece antes.
        QObject::connect(&proc, &QProcess::readyRead, &proc,
                         [&proc, &log]() { log += QString::fromLocal8Bit(proc.readAll()); });

        log.clear();
        QFile::remove(rawObj);
        proc.start();
        if (!proc.waitForStarted(10000))
            return false;
        if (!proc.waitForFinished(timeoutMs)) {
            proc.kill();
            proc.waitForFinished(3000);
            return false;
        }
        log += QString::fromLocal8Bit(proc.readAll());
        return proc.exitCode() == 0 && QFileInfo::exists(rawObj)
               && QFileInfo(rawObj).size() > 64;
    };

    if (!runBlender(pyModern)) {
        QFile::remove(rawObj);
        if (!runBlender(pyOld)) {
            // Só a cauda: o log do Blender em modo batch é enorme e a
            // status bar não comporta.
            QString tail = log.trimmed();
            if (tail.size() > 900) tail = QStringLiteral("…") + tail.right(900);
            QStringList lines = tail.split(QLatin1Char('\n'));
            while (!lines.isEmpty() && lines.first().trimmed().isEmpty())
                lines.removeFirst();
            if (lines.size() > 12)
                lines = lines.mid(lines.size() - 12);
            tail = lines.join(QLatin1Char('\n'));
            return fail(QStringLiteral(
                "Falha ao exportar .blend com o Blender.\n"
                "Arquivo: %1\n"
                "Blender: %2\n"
                "Dica: abra o .blend no Blender e exporte .obj/.glb manualmente.%2")
                            .arg(blendPath, blender,
                                 tail.isEmpty() ? QString()
                                                : QStringLiteral("\n\nSaída do Blender:\n") + tail));
        }
    }

    // O .obj exportado precisa sobreviver ao reboot: é ele que vai para
    // Track::meshPath. Escrever em QDir::temp() (como estava) fazia o projeto
    // quebrar em silêncio quando o /tmp era limpo.
    // Preferência: do lado do .blend de origem, que é onde o usuário espera
    // achar. Fallback: AppData, para .blend vindo de mídia somente-leitura.
    QString dir = outDir.isEmpty() ? QFileInfo(blendPath).absolutePath() : outDir;
    if (!QDir().mkpath(dir) || !QFileInfo(dir).isWritable()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
              + QStringLiteral("/blend-cache");
        if (!QDir().mkpath(dir))
            return fail(QStringLiteral("Não criou o diretório de malhas:\n%1").arg(dir));
    }

    const QString base = QFileInfo(blendPath).completeBaseName();
    const QString stable =
        QDir(dir).filePath(QStringLiteral("%1.blend.obj").arg(base));
    if (QFile::exists(stable) && !QFile::remove(stable))
        return fail(QStringLiteral("Não substituiu a malha anterior:\n%1").arg(stable));
    if (!QFile::copy(rawObj, stable))
        return fail(QStringLiteral("Não copiou o .obj exportado para:\n%1").arg(stable));

    // O Blender escreve o .mtl e as texturas ao lado do .obj. Copiar só o .obj
    // deixaria o material órfão, então o diretório inteiro vem junto.
    const QString srcDir = QFileInfo(blendPath).absolutePath();
    const QString srcMtl = QDir(srcDir).filePath(base + QStringLiteral(".mtl"));
    if (QFileInfo::exists(srcMtl)) {
        const QString dstMtl = QDir(dir).filePath(base + QStringLiteral(".mtl"));
        QFile::remove(dstMtl);
        QFile::copy(srcMtl, dstMtl);
    }
    const QStringList extra = QDir(srcDir).entryList(
        {QStringLiteral("%1*.png").arg(base), QStringLiteral("%1*.jpg").arg(base),
         QStringLiteral("%1*.jpeg").arg(base)},
        QDir::Files);
    for (const QString& f : extra) {
        const QString dst = QDir(dir).filePath(f);
        if (QFile::exists(dst)) QFile::remove(dst);
        QFile::copy(QDir(srcDir).filePath(f), dst);
    }
    return stable;
}

} // namespace mesh
