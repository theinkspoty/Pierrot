// Pierrot — editor de vídeo
// Copyright (C) 2026 theinkspoty
// SPDX-License-Identifier: GPL-3.0-or-later
// Licenciado sob a GNU GPL v3 ou superior. Veja LICENSE.

#include "BlenderBridge.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace mesh {

namespace {

// Prefixo do comando que lança o Blender (programa + args iniciais). Vazio =
// não encontrado. Ordem: PIERROT_BLENDER > "blender" no PATH > Flatpak.
// O Flatpak expõe o binário como "org.blender.Blender", não "blender" — sem
// esta etapa ele nunca era encontrado numa distro instalada só via Flatpak.
QStringList blenderPrefix() {
    // Variáveis de ambiente têm precedência sobre o PATH: quem usa Blender
    // portable ou um build em /opt precisa de um jeito de apontar pra ele.
    const QByteArray env = qgetenv("PIERROT_BLENDER");
    if (!env.isEmpty()) {
        const QString p = QFileInfo(QString::fromLocal8Bit(env)).absoluteFilePath();
        if (QFileInfo::exists(p)) return {p};
    }
    const QString exe = QStandardPaths::findExecutable(QStringLiteral("blender"));
    if (!exe.isEmpty()) return {exe};
    const QString flatpak = QStandardPaths::findExecutable(QStringLiteral("flatpak"));
    if (!flatpak.isEmpty()) {
        QProcess p;
        p.start(flatpak, {QStringLiteral("info"), QStringLiteral("org.blender.Blender")});
        if (p.waitForFinished(5000) && p.exitCode() == 0)
            return {flatpak, QStringLiteral("run"), QStringLiteral("org.blender.Blender")};
    }
    return {};
}

} // namespace

bool blenderAvailable(QString* resolvedPath) {
    const QStringList prefix = blenderPrefix();
    if (prefix.isEmpty()) {
        if (resolvedPath) resolvedPath->clear();
        return false;
    }
    if (resolvedPath) *resolvedPath = prefix.join(QLatin1Char(' '));
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

    const QStringList blender = blenderPrefix();
    if (blender.isEmpty())
        return fail(QStringLiteral(
            "Blender não encontrado.\n"
            "Procurado em PATH, PIERROT_BLENDER e Flatpak "
            "(org.blender.Blender). Instale o Blender "
            "(https://www.blender.org) ou exporte .obj/.glb manualmente."));

    // O .obj exportado precisa sobreviver ao reboot: é ele que vai para
    // Track::meshPath. Escrever em QDir::temp() (como estava) fazia o projeto
    // quebrar em silêncio quando o /tmp era limpo.
    // Preferência: do lado do .blend de origem, que é onde o usuário espera
    // achar. Fallback: AppData, para .blend vindo de mídia somente-leitura.
    // O temp de export também mora aqui (e não em /tmp): o Blender do
    // Flatpak tem /tmp privado e não enxergaria o diretório temporário do
    // host — os artefatos precisam sair num caminho compartilhado.
    QString dir = outDir.isEmpty() ? QFileInfo(blendPath).absolutePath() : outDir;
    if (!QDir().mkpath(dir) || !QFileInfo(dir).isWritable()) {
        dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
              + QStringLiteral("/blend-cache");
        if (!QDir().mkpath(dir))
            return fail(QStringLiteral("Não criou o diretório de malhas:\n%1").arg(dir));
    }

    QTemporaryDir tmp(QDir(dir).filePath(QStringLiteral(".pierrot-export-XXXXXX")));
    if (!tmp.isValid())
        return fail(QStringLiteral("Não criou diretório temporário em:\n%1").arg(dir));
    const QString rawObj = tmp.filePath(QStringLiteral("pierrot_export.obj"));
    const QString rawMtl = tmp.filePath(QStringLiteral("pierrot_export.mtl"));

    // Comando final. No Flatpak, os diretórios do .blend e da saída são
    // expostos ao sandbox explicitamente — sem isso o export falharia para
    // arquivos fora de home.
    QStringList cmd = blender;
    if (cmd.size() >= 3 && cmd[1] == QLatin1String("run")) {
        QStringList pre = {cmd[0], cmd[1]};
        const QStringList expostos = {QFileInfo(blendPath).absolutePath(), dir};
        for (const QString& d : expostos) {
            const QString opt = QStringLiteral("--filesystem=") + d;
            if (!pre.contains(opt)) pre << opt;
        }
        cmd = pre + cmd.mid(2);
    }

    // O script tenta o exportador novo (wm.obj_export, Blender 3.3+) e cai
    // pro antigo (export_scene.obj). O novo é chamado com uma lista de
    // kwargs decrescentes: um TypeError (kwarg que a versão não conhece)
    // desce um nível sem reiniciar o Blender, e o último nível roda sem
    // kwargs — aceito em todas as versões.
    //
    // use_selection=False é explícito de propósito: o Blender 4.x entrega com
    // nada selecionado ao abrir o .blend, e sem isso a cena inteira sairia
    // vazia. export_materials=True traz o .mtl, e path_mode='COPY' manda o
    // Blender copiar as texturas para junto do .obj exportado — sem isso as
    // imagens ficam só nos paths originais e somem quando o .obj vai pro
    // diretório do projeto.
    const QString pyModern = QStringLiteral(
        "import bpy, sys\n"
        "out = sys.argv[-1]\n"
        "try:\n"
        "    attempts = [\n"
        "        dict(use_selection=False, export_materials=True,\n"
        "             apply_modifiers=True, path_mode='COPY'),\n"
        "        dict(use_selection=False, export_materials=True,\n"
        "             apply_modifiers=True),\n"
        "        dict(export_materials=True, apply_modifiers=True),\n"
        "        dict(),\n"
        "    ]\n"
        "    for kw in attempts:\n"
        "        try:\n"
        "            bpy.ops.wm.obj_export(filepath=out, **kw)\n"
        "            break\n"
        "        except TypeError:\n"
        "            continue\n"
        "    else:\n"
        "        raise RuntimeError('wm.obj_export sem kwargs compativeis')\n"
        "except Exception:\n"
        "    try:\n"
        "        bpy.ops.export_scene.obj(filepath=out, path_mode='COPY')\n"
        "    except TypeError:\n"
        "        bpy.ops.export_scene.obj(filepath=out)\n");
    const QString pyOld = QStringLiteral(
        "import bpy, sys\n"
        "out = sys.argv[-1]\n"
        "try:\n"
        "    bpy.ops.export_scene.obj(filepath=out, path_mode='COPY')\n"
        "except TypeError:\n"
        "    bpy.ops.export_scene.obj(filepath=out)\n");

    // O Blender 5.x escreve map_Kd só para texturas ligadas a um Principled
    // BSDF — material com Diffuse BSDF (comum em .blend antigos, ex. "Tin
    // blend") exporta o .mtl SEM nenhum map_Kd e a malha sai sem textura.
    // Depois de exportar, o script completa o .mtl: para cada material sem
    // map_Kd, segue os nodes a partir do output, acha a imagem conectada e
    // acrescenta `map_Kd <abs>` no bloco newmtl dela. A ponte (C++) resolve
    // o path absoluto e copia a textura pro lado do .obj como de costume.
    const QString pyAugment = QStringLiteral(
        "def _pierrot_fill_maps():\n"
        "    import os\n"
        "    mtl = os.path.splitext(out)[0] + '.mtl'\n"
        "    if not os.path.exists(mtl):\n"
        "        return\n"
        "    lines = open(mtl, encoding='utf-8', errors='replace').read().splitlines()\n"
        "    has = {}\n"
        "    cur = None\n"
        "    for ln in lines:\n"
        "        s = ln.strip()\n"
        "        if s.startswith('newmtl '):\n"
        "            cur = s[7:].strip()\n"
        "            has[cur] = False\n"
        "        elif s.startswith('map_Kd') and cur is not None:\n"
        "            has[cur] = True\n"
        "    need = {}\n"
        "    for m in bpy.data.materials:\n"
        "        if m.name not in has or has[m.name]:\n"
        "            continue\n"
        "        if not getattr(m, 'use_nodes', False) or not m.node_tree:\n"
        "            continue\n"
        "        stack = []\n"
        "        for n in m.node_tree.nodes:\n"
        "            if n.type == 'OUTPUT_MATERIAL':\n"
        "                for i in n.inputs:\n"
        "                    if i.is_linked:\n"
        "                        stack.append(i.links[0].from_node)\n"
        "        img = None\n"
        "        seen = set()\n"
        "        while stack:\n"
        "            n = stack.pop()\n"
        "            if id(n) in seen:\n"
        "                continue\n"
        "            seen.add(id(n))\n"
        "            if n.type == 'TEX_IMAGE' and n.image:\n"
        "                img = n.image\n"
        "                break\n"
        "            for i in n.inputs:\n"
        "                if i.is_linked:\n"
        "                    stack.append(i.links[0].from_node)\n"
        "        if img is None:\n"
        "            continue\n"
        "        try:\n"
        "            p = bpy.path.abspath(img.filepath, library=img.library)\n"
        "        except Exception:\n"
        "            p = bpy.path.abspath(img.filepath)\n"
        "        if p and os.path.exists(p):\n"
        "            need[m.name] = p\n"
        "    if not need:\n"
        "        return\n"
        "    outl = []\n"
        "    cur = None\n"
        "    for ln in lines:\n"
        "        s = ln.strip()\n"
        "        if s.startswith('newmtl '):\n"
        "            if cur in need:\n"
        "                outl.append('map_Kd ' + need[cur])\n"
        "            cur = s[7:].strip()\n"
        "        outl.append(ln)\n"
        "    if cur in need:\n"
        "        outl.append('map_Kd ' + need[cur])\n"
        "    open(mtl, 'w', encoding='utf-8').write('\\n'.join(outl) + '\\n')\n"
        "try:\n"
        "    _pierrot_fill_maps()\n"
        "except Exception:\n"
        "    pass\n");

    QString log;
    auto runBlender = [&](const QString& py) -> bool {
        QProcess proc;
        proc.setProgram(cmd.first());
        // blender -b file.blend --python-expr "..." -- /tmp/out.obj
        // O último argv é o path do .obj (sys.argv[-1] no script).
        QStringList args = cmd.mid(1);
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
        QFile::remove(rawMtl);
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

    if (!runBlender(pyModern + pyAugment)) {
        QFile::remove(rawObj);
        if (!runBlender(pyOld + pyAugment)) {
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
                "Dica: abra o .blend no Blender e exporte .obj/.glb manualmente.%3")
                            .arg(blendPath, blender.join(QLatin1Char(' ')),
                                 tail.isEmpty() ? QString()
                                                : QStringLiteral("\n\nSaída do Blender:\n") + tail));
        }
    }

    const QString base = QFileInfo(blendPath).completeBaseName();
    const QString mtlName = base + QStringLiteral(".blend.mtl");
    const QString stable =
        QDir(dir).filePath(QStringLiteral("%1.blend.obj").arg(base));
    const QString stableMtl = QDir(dir).filePath(mtlName);
    if (QFile::exists(stable) && !QFile::remove(stable))
        return fail(QStringLiteral("Não substituiu a malha anterior:\n%1").arg(stable));

    // Cópia com reescrita do mtllib: o .obj gerado no temp cita
    // "pierrot_export.mtl", que deixa de existir depois da cópia — sem
    // apontar pro nome final o ObjLoader nunca acha o material.
    {
        QFile in(rawObj);
        if (!in.open(QIODevice::ReadOnly))
            return fail(QStringLiteral("Não abriu o .obj exportado:\n%1").arg(rawObj));
        QFile out(stable);
        if (!out.open(QIODevice::WriteOnly)) {
            in.close();
            return fail(QStringLiteral("Não criou o .obj:\n%1").arg(stable));
        }
        const QByteArray tag = "pierrot_export.mtl";
        while (!in.atEnd()) {
            QByteArray line = in.readLine();
            if (line.startsWith("mtllib "))
                line.replace(tag, mtlName.toUtf8());
            out.write(line);
        }
    }

    // Texturas referenciadas no .mtl: o Blender escreve o caminho em três
    // formas (absoluto, "//relativo ao blend" ou o nome que está no temp com
    // path_mode='COPY'). Resolve em qualquer uma, copia o arquivo pro
    // diretório final e deixa a linha com nome simples — o ObjLoader procura
    // ao lado do .obj, então path absoluto de outra máquina nunca serviria.
    const auto resolveTex = [&](const QString& mapKdIn) -> QString {
        QString p = mapKdIn.trimmed();
        p.replace(QLatin1Char('\\'), QLatin1Char('/'));
        if (p.isEmpty()) return QString();
        const QString blendDir = QFileInfo(blendPath).absolutePath();
        QStringList cand;
        if (p.startsWith(QLatin1String("//")))
            cand << QDir(blendDir).filePath(p.mid(2)); // "//" = relativo ao .blend
        else {
            cand << p;                          // absoluto
            cand << QDir(tmp.path()).filePath(p);   // ao lado do .obj no temp
            cand << QDir(blendDir).filePath(p); // relativo ao .blend
        }
        for (const QString& c : cand)
            if (QFileInfo::exists(c)) return QFileInfo(c).absoluteFilePath();
        // Último recurso: mesmo nome em qualquer subdir do temp.
        const QString want = QFileInfo(p).fileName();
        QDirIterator it(tmp.path(), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString f = it.next();
            if (QFileInfo(f).fileName() == want) return QFileInfo(f).absoluteFilePath();
        }
        return QString();
    };

    // O .obj/.mtl exportados precisam sobreviver ao reboot: é Track::meshPath
    // que aponta pra eles (escrever em /tmp quebrava o projeto em silêncio).
    bool mtlCopied = false;
    if (QFileInfo::exists(rawMtl)) {
        QFile in(rawMtl);
        if (!in.open(QIODevice::ReadOnly | QIODevice::Text))
            return fail(QStringLiteral("Não abriu o .mtl exportado:\n%1").arg(rawMtl));
        if (QFile::exists(stableMtl)) QFile::remove(stableMtl);
        QFile out(stableMtl);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Text)) {
            in.close();
            return fail(QStringLiteral("Não criou o .mtl:\n%1").arg(stableMtl));
        }
        while (!in.atEnd()) {
            QByteArray line = in.readLine();
            if (line.startsWith("map_Kd")) {
                const QString val =
                    QString::fromUtf8(line.mid(6)).trimmed(); // resto da linha
                const QString src = resolveTex(val);
                if (!src.isEmpty()) {
                    const QString dstName = QFileInfo(src).fileName();
                    const QString dst = QDir(dir).filePath(dstName);
                    if (QFileInfo(src).absoluteFilePath()
                        != QFileInfo(dst).absoluteFilePath()) {
                        QFile::remove(dst);
                        // Cópia falhou: mantém a linha original — o path
                        // antigo ainda pode resolver no loader.
                        if (QFile::copy(src, dst)) {
                            line = "map_Kd " + dstName.toUtf8() + '\n';
                        }
                    } else {
                        line = "map_Kd " + dstName.toUtf8() + '\n';
                    }
                }
            }
            out.write(line);
        }
        mtlCopied = true;
    }

    // Fallback legado: .mtl/texturas escritos à mão ao lado do .blend (o
    // exportador não gerou material, ex. cena sem materiais). Entra com o
    // MESMO nome do mtllib já reescrito no .obj, senão o loader procura um
    // arquivo que não existe.
    if (!mtlCopied) {
        const QString srcDir = QFileInfo(blendPath).absolutePath();
        const QString srcMtl =
            QDir(srcDir).filePath(base + QStringLiteral(".mtl"));
        if (QFileInfo::exists(srcMtl)) {
            QFile::remove(stableMtl);
            QFile::copy(srcMtl, stableMtl);
        }
        const QStringList extra = QDir(srcDir).entryList(
            {QStringLiteral("%1*.png").arg(base), QStringLiteral("%1*.jpg").arg(base),
             QStringLiteral("%1*.jpeg").arg(base)},
            QDir::Files);
        for (const QString& f : extra) {
            const QString src = QDir(srcDir).filePath(f);
            const QString dst = QDir(dir).filePath(f);
            // src == dst quando outDir é a própria pasta do .blend: o
            // remove+copy abaixo APAGARIA a textura e falharia em seguida.
            if (QFileInfo(src).absoluteFilePath()
                == QFileInfo(dst).absoluteFilePath())
                continue;
            if (QFile::exists(dst)) QFile::remove(dst);
            QFile::copy(src, dst);
        }
    }
    return stable;
}

} // namespace mesh
