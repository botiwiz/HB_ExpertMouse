// Preview of one file: big image (HB Viewer's own decoder for pictures, so even huge PSDs
// are instant; KIO thumbnailers for videos/PDFs/documents; the type icon otherwise) + info.
// Used by the cascade's last column and by the browser's right-hand preview panel.
#pragma once
#include <KFileItem>
#include <QPixmap>
#include <QWidget>

class QLabel;

class PreviewPane : public QWidget {
    Q_OBJECT
public:
    explicit PreviewPane(QWidget* parent = nullptr);
    void showItem(const KFileItem& item);
    void clearItem();
    KFileItem item() const { return item_; }

protected:
    void resizeEvent(QResizeEvent*) override;

private:
    void setPicture(const QPixmap& pm);
    void refit();

    QLabel *image_, *info_;
    QPixmap full_;          // unscaled preview, rescaled to the pane width
    KFileItem item_;
    QString baseInfo_;
    int gen_ = 0;
};
