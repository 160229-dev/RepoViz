#include "widget.h"
#include "ui_widget.h"

#include <QFileDialog>
#include <QMessageBox>
#include <QHeaderView>
#include <QTableWidgetItem>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QDir>
#include <QUrl>
#include <QDateTime>
#include <QRegularExpression>

Widget::Widget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Widget)
{
    ui->setupUi(this);

    // Initialize network manager
    m_networkManager = new QNetworkAccessManager(this);

    // Page switching
    connect(ui->listWidget, &QListWidget::currentRowChanged,
            this, &Widget::onPageChanged);

    // Clone signals
    connect(ui->browseButton, &QPushButton::clicked,
            this, &Widget::onBrowseClicked);
    connect(ui->cloneButton, &QPushButton::clicked,
            this, &Widget::onCloneClicked);

    // Data fetch signals
    connect(ui->fetchButton, &QPushButton::clicked,
            this, &Widget::onFetchClicked);

    // Default selection
    ui->listWidget->setCurrentRow(0);

    // Default clone destination: user home
    ui->clonePathEdit->setText(QDir::homePath());

    // Configure result table
    ui->resultTable->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    ui->resultTable->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::Stretch);
    ui->resultTable->verticalHeader()->setVisible(false);
    ui->resultTable->verticalHeader()->setDefaultSectionSize(20);
    ui->resultTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->resultTable->setAlternatingRowColors(true);
    ui->resultTable->setShowGrid(false);
    ui->resultTable->setWordWrap(false);

    // Configure releases table
    ui->releasesTable->horizontalHeader()->setSectionResizeMode(
        0, QHeaderView::ResizeToContents);
    ui->releasesTable->horizontalHeader()->setSectionResizeMode(
        1, QHeaderView::ResizeToContents);
    ui->releasesTable->horizontalHeader()->setSectionResizeMode(
        2, QHeaderView::Stretch);
    ui->releasesTable->verticalHeader()->setVisible(false);
    ui->releasesTable->verticalHeader()->setDefaultSectionSize(20);
    ui->releasesTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    ui->releasesTable->setAlternatingRowColors(true);
    ui->releasesTable->setShowGrid(false);
    ui->releasesTable->setWordWrap(false);

    // Initial progress bar state: determinate, 0%
    ui->progressBar->setRange(0, 100);
    ui->progressBar->setValue(0);
    setStatus("Idle");
}

Widget::~Widget()
{
    if (m_cloneProcess && m_cloneProcess->state() != QProcess::NotRunning) {
        m_cloneProcess->kill();
        m_cloneProcess->waitForFinished();
    }
    delete ui;
}

void Widget::onPageChanged(int index)
{
    ui->stackedWidget->setCurrentIndex(index);
}

void Widget::setControlsEnabled(bool enabled)
{
    ui->repoUrlEdit->setEnabled(enabled);
    ui->clonePathEdit->setEnabled(enabled);
    ui->browseButton->setEnabled(enabled);
    ui->cloneButton->setEnabled(enabled);
}

void Widget::setStatus(const QString &text)
{
    ui->statusLabel->setText(text);
}

void Widget::resetProgress(bool indeterminate)
{
    if (indeterminate) {
        ui->progressBar->setRange(0, 0);
    } else {
        ui->progressBar->setRange(0, 100);
        ui->progressBar->setValue(0);
    }
    m_progressDeterminate = !indeterminate;
}

void Widget::parseGitProgress(const QByteArray &data)
{
    // Git --progress output includes lines like:
    //   "Receiving objects:  45% (567/1234)"
    //   "Resolving deltas:  100% (123/123)"
    //   "Receiving objects: 100% (1234/1234), 2.34 MiB | 1.23 MiB/s, done."
    static const QRegularExpression rx(
        "(Receiving objects|Resolving deltas|Total|remote: Compressing)"
        "\\s*[:]?\\s*(\\d{1,3})%",
        QRegularExpression::CaseInsensitiveOption);

    const QString text = QString::fromLocal8Bit(data);
    int lastPct = -1;
    auto it = rx.globalMatch(text);
    while (it.hasNext()) {
        const auto m = it.next();
        bool ok = false;
        const int pct = m.captured(2).toInt(&ok);
        if (ok && pct >= 0 && pct <= 100) {
            lastPct = pct;
        }
    }
    if (lastPct < 0) return;

    if (!m_progressDeterminate) {
        ui->progressBar->setRange(0, 100);
        m_progressDeterminate = true;
    }
    // Receiving objects can go up to ~90%, then resolving deltas 90-100%
    ui->progressBar->setValue(lastPct);
}

void Widget::onBrowseClicked()
{
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select destination directory"), ui->clonePathEdit->text());
    if (!dir.isEmpty()) {
        ui->clonePathEdit->setText(dir);
    }
}

void Widget::onCloneClicked()
{
    const QString url = ui->repoUrlEdit->text().trimmed();
    const QString path = ui->clonePathEdit->text().trimmed();

    if (url.isEmpty()) {
        QMessageBox::warning(this, tr("Missing input"), tr("Please enter a repository URL."));
        return;
    }
    if (path.isEmpty()) {
        QMessageBox::warning(this, tr("Missing input"), tr("Please choose a destination directory."));
        return;
    }

    QDir targetDir(path);
    if (!targetDir.exists()) {
        QMessageBox::warning(this, tr("Invalid path"),
                             tr("The destination directory does not exist:\n%1").arg(path));
        return;
    }

    if (m_cloneProcess) {
        m_cloneProcess->deleteLater();
    }
    m_cloneProcess = new QProcess(this);
    m_cloneProcess->setWorkingDirectory(path);
    // Force progress output to be on stderr, line-buffered.
    m_cloneProcess->setProcessChannelMode(QProcess::SeparateChannels);

    connect(m_cloneProcess, &QProcess::readyReadStandardOutput,
            this, &Widget::onCloneOutput);
    connect(m_cloneProcess, &QProcess::readyReadStandardError,
            this, &Widget::onCloneOutput);
    connect(m_cloneProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &Widget::onCloneFinished);
    connect(m_cloneProcess, &QProcess::errorOccurred,
            this, &Widget::onCloneError);

    setControlsEnabled(false);
    resetProgress(true);
    setStatus(QString("Cloning %1 ...").arg(url));

    QStringList arguments;
    arguments << "clone" << "--progress" << url;
    m_cloneProcess->start("git", arguments);
}

void Widget::onCloneOutput()
{
    if (!m_cloneProcess) return;
    const QByteArray out = m_cloneProcess->readAllStandardOutput();
    const QByteArray err = m_cloneProcess->readAllStandardError();
    if (!out.isEmpty()) parseGitProgress(out);
    if (!err.isEmpty()) {
        parseGitProgress(err);
        // Update status with a short, latest meaningful line.
        const QString line = QString::fromLocal8Bit(err).trimmed();
        const QStringList parts = line.split('\n', Qt::SkipEmptyParts);
        if (!parts.isEmpty()) {
            setStatus(parts.last().left(120));
        }
    }
}

void Widget::onCloneFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    setControlsEnabled(true);
    if (exitStatus == QProcess::CrashExit) {
        resetProgress(false);
        ui->progressBar->setValue(0);
        setStatus("Clone process crashed.");
    } else if (exitCode != 0) {
        resetProgress(false);
        ui->progressBar->setValue(0);
        setStatus(QString("Clone failed (exit code %1).").arg(exitCode));
        QMessageBox::critical(this, tr("Clone failed"),
                              tr("Please make sure git is installed and the URL is correct."));
    } else {
        resetProgress(false);
        ui->progressBar->setValue(100);
        setStatus("Clone completed successfully.");
    }
}

void Widget::onCloneError(QProcess::ProcessError error)
{
    setControlsEnabled(true);
    QString msg;
    switch (error) {
    case QProcess::FailedToStart:
        msg = tr("Could not start git. Please make sure git is installed and added to PATH.");
        break;
    case QProcess::Crashed:        msg = tr("Clone process crashed."); break;
    case QProcess::Timedout:       msg = tr("Process timed out."); break;
    case QProcess::WriteError:
    case QProcess::ReadError:      msg = tr("Process I/O error."); break;
    default:                       msg = tr("Unknown error."); break;
    }
    resetProgress(false);
    ui->progressBar->setValue(0);
    setStatus(msg);
    QMessageBox::critical(this, tr("Error"), msg);
}

void Widget::onFetchClicked()
{
    const QString user = ui->userNameEdit->text().trimmed();
    const QString repo = ui->repoNameEdit->text().trimmed();

    if (user.isEmpty() || repo.isEmpty()) {
        QMessageBox::warning(this, tr("Missing input"),
                             tr("Please enter both a GitHub username and a repository name."));
        return;
    }

    ui->resultTable->setRowCount(0);
    ui->releasesTable->setRowCount(0);
    addRow(ui->resultTable, "Status", QString("Fetching %1/%2 ...").arg(user, repo));

    m_repoInfo = QJsonObject();
    m_totalDownloadCount = 0;
    m_totalReleases = 0;
    m_allReleases = QJsonArray();

    QUrl repoUrl(QString("https://api.github.com/repos/%1/%2").arg(user, repo));
    QNetworkRequest req(repoUrl);
    req.setHeader(QNetworkRequest::UserAgentHeader, "Qt-GitHub-Manager");
    QNetworkReply *reply = m_networkManager->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onRepoInfoReply(reply);
    });
}

void Widget::onRepoInfoReply(QNetworkReply *reply)
{
    if (reply->error() != QNetworkReply::NoError) {
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        QString err = reply->errorString();
        if (status == 404) {
            err = "Repository not found. Please check the username and repository name.";
        } else if (status == 403) {
            err = "API rate limit exceeded. Please wait and try again (60 requests/hour for unauthenticated users).";
        } else if (status == 0) {
            err = "Network error: " + err;
        }
        ui->resultTable->setRowCount(0);
        addRow(ui->resultTable, "Error", err);
        reply->deleteLater();
        return;
    }

    const QByteArray data = reply->readAll();
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isObject()) {
        ui->resultTable->setRowCount(0);
        addRow(ui->resultTable, "Error", "Failed to parse response: " + perr.errorString());
        reply->deleteLater();
        return;
    }

    m_repoInfo = doc.object();
    reply->deleteLater();

    // Fetch releases to compute downloads.
    const QString user = m_repoInfo.value("owner").toObject().value("login").toString();
    const QString repo = m_repoInfo.value("name").toString();
    if (user.isEmpty() || repo.isEmpty()) {
        return;
    }

    QUrl releasesUrl(QString("https://api.github.com/repos/%1/%2/releases?per_page=100")
                         .arg(user, repo));
    QNetworkRequest rreq(releasesUrl);
    rreq.setHeader(QNetworkRequest::UserAgentHeader, "Qt-GitHub-Manager");
    QNetworkReply *rreply = m_networkManager->get(rreq);
    connect(rreply, &QNetworkReply::finished, this, [this, rreply]() {
        onReleasesReply(rreply);
    });
}

void Widget::onReleasesReply(QNetworkReply *reply)
{
    if (reply->error() == QNetworkReply::NoError) {
        const QByteArray data = reply->readAll();
        QJsonParseError perr;
        const QJsonDocument doc = QJsonDocument::fromJson(data, &perr);
        if (perr.error == QJsonParseError::NoError && doc.isArray()) {
            m_allReleases = doc.array();
            m_totalReleases = m_allReleases.size();
            for (const QJsonValue &v : m_allReleases) {
                const QJsonArray assets = v.toObject().value("assets").toArray();
                for (const QJsonValue &av : assets) {
                    m_totalDownloadCount += av.toObject().value("download_count").toInt();
                }
            }
        }
    }
    reply->deleteLater();
    populateResultTable();
    populateReleasesTable();
}

void Widget::addRow(QTableWidget *table, const QString &key, const QString &value)
{
    const int row = table->rowCount();
    table->insertRow(row);
    auto *keyItem = new QTableWidgetItem(key);
    auto *valItem = new QTableWidgetItem(value);
    keyItem->setFlags(keyItem->flags() & ~Qt::ItemIsEditable);
    valItem->setFlags(valItem->flags() & ~Qt::ItemIsEditable);
    QFont f = keyItem->font();
    f.setBold(true);
    keyItem->setFont(f);
    table->setItem(row, 0, keyItem);
    table->setItem(row, 1, valItem);
}

void Widget::populateResultTable()
{
    ui->resultTable->setRowCount(0);

    auto formatDate = [](const QString &iso) {
        if (iso.isEmpty()) return QString();
        QDateTime dt = QDateTime::fromString(iso, Qt::ISODate);
        if (!dt.isValid()) return iso;
        return dt.toString("yyyy-MM-dd HH:mm:ss");
    };

    auto str = [](const QJsonValue &v) { return v.toVariant().toString(); };

    addRow(ui->resultTable, "Repository",
           m_repoInfo.value("full_name").toString());
    addRow(ui->resultTable, "Description",
           m_repoInfo.value("description").toString().isEmpty()
               ? "(none)" : m_repoInfo.value("description").toString());
    addRow(ui->resultTable, "URL",
           m_repoInfo.value("html_url").toString());

    addRow(ui->resultTable, "Stars",         str(m_repoInfo.value("stargazers_count")));
    addRow(ui->resultTable, "Watchers",      str(m_repoInfo.value("subscribers_count")));
    addRow(ui->resultTable, "Forks",         str(m_repoInfo.value("forks_count")));
    addRow(ui->resultTable, "Open Issues",   str(m_repoInfo.value("open_issues_count")));
    addRow(ui->resultTable, "Total Downloads (Release assets)",
           QString::number(m_totalDownloadCount));
    addRow(ui->resultTable, "Releases",      QString::number(m_totalReleases));

    addRow(ui->resultTable, "Primary Language",
           m_repoInfo.value("language").toString().isEmpty()
               ? "Unknown" : m_repoInfo.value("language").toString());
    addRow(ui->resultTable, "License",
           m_repoInfo.value("license").toObject().value("spdx_id").toString().isEmpty()
               ? "Not specified" : m_repoInfo.value("license").toObject().value("spdx_id").toString());
    addRow(ui->resultTable, "Default Branch",
           m_repoInfo.value("default_branch").toString());
    addRow(ui->resultTable, "Size (KB)",     str(m_repoInfo.value("size")));
    addRow(ui->resultTable, "Visibility",
           m_repoInfo.value("private").toBool() ? "Private" : "Public");
    addRow(ui->resultTable, "Is Fork",       m_repoInfo.value("fork").toBool() ? "Yes" : "No");
    addRow(ui->resultTable, "Is Archived",   m_repoInfo.value("archived").toBool() ? "Yes" : "No");
    addRow(ui->resultTable, "Created",       formatDate(m_repoInfo.value("created_at").toString()));
    addRow(ui->resultTable, "Last Updated",  formatDate(m_repoInfo.value("updated_at").toString()));
    addRow(ui->resultTable, "Last Push",     formatDate(m_repoInfo.value("pushed_at").toString()));
}

void Widget::populateReleasesTable()
{
    ui->releasesTable->setRowCount(0);

    auto formatDate = [](const QString &iso) {
        if (iso.isEmpty()) return QString();
        QDateTime dt = QDateTime::fromString(iso, Qt::ISODate);
        if (!dt.isValid()) return iso;
        return dt.toString("yyyy-MM-dd HH:mm:ss");
    };

    const int show = qMin(10, m_totalReleases);
    for (int i = 0; i < show; ++i) {
        const QJsonObject rel = m_allReleases.at(i).toObject();
        const QString tag = rel.value("tag_name").toString();
        const QString published = formatDate(rel.value("published_at").toString());
        const QJsonArray assets = rel.value("assets").toArray();
        qint64 dl = 0;
        for (const QJsonValue &av : assets) {
            dl += av.toObject().value("download_count").toInt();
        }
        const int row = ui->releasesTable->rowCount();
        ui->releasesTable->insertRow(row);
        ui->releasesTable->setItem(row, 0, new QTableWidgetItem(tag));
        ui->releasesTable->setItem(row, 1, new QTableWidgetItem(published));
        ui->releasesTable->setItem(row, 2, new QTableWidgetItem(QString::number(dl)));
        for (int c = 0; c < 3; ++c) {
            auto *it = ui->releasesTable->item(row, c);
            it->setFlags(it->flags() & ~Qt::ItemIsEditable);
        }
    }

    if (show == 0) {
        const int row = ui->releasesTable->rowCount();
        ui->releasesTable->insertRow(row);
        auto *item = new QTableWidgetItem("No releases found.");
        item->setFlags(item->flags() & ~Qt::ItemIsEditable);
        ui->releasesTable->setItem(row, 0, item);
        ui->releasesTable->setSpan(row, 0, 1, 3);
    }
}
