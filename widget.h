#ifndef WIDGET_H
#define WIDGET_H

#include <QWidget>
#include <QProcess>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QJsonObject>
#include <QJsonArray>
#include <QTableWidget>

QT_BEGIN_NAMESPACE
namespace Ui {
class Widget;
}
QT_END_NAMESPACE

class Widget : public QWidget
{
    Q_OBJECT

public:
    explicit Widget(QWidget *parent = nullptr);
    ~Widget() override;

private slots:
    // Page switching
    void onPageChanged(int index);

    // Clone related
    void onBrowseClicked();
    void onCloneClicked();
    void onCloneOutput();
    void onCloneFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onCloneError(QProcess::ProcessError error);

    // Data fetching related
    void onFetchClicked();
    void onRepoInfoReply(QNetworkReply *reply);
    void onReleasesReply(QNetworkReply *reply);

private:
    void setControlsEnabled(bool enabled);
    void setStatus(const QString &text);
    void resetProgress(bool indeterminate);
    void parseGitProgress(const QByteArray &data);
    void populateResultTable();
    void populateReleasesTable();
    void addRow(QTableWidget *table, const QString &key, const QString &value);

    Ui::Widget *ui;
    QProcess *m_cloneProcess = nullptr;
    QNetworkAccessManager *m_networkManager = nullptr;

    // Cached repository data
    QJsonObject m_repoInfo;
    int m_totalReleases = 0;
    qint64 m_totalDownloadCount = 0;
    QJsonArray m_allReleases;
    bool m_progressDeterminate = false;
};

#endif // WIDGET_H
