#ifndef KEYBOARDVIEW_H
#define KEYBOARDVIEW_H

#include <QAudioOutput>
#include <QElapsedTimer>
#include <QScrollArea>
#include <QWidget>
#include <optional>

#include "../Clipboard.h"
#include "../EditState.h"
#include "Animation.h"
#include "MooClock.h"
#include "editor/PxtoneClient.h"
#include "editor/UnitListModel.h"
#include "editor/audio/NotePreview.h"
#include "pxtone/pxtnService.h"

enum struct Direction { UP, DOWN };

struct LocalEditState {
  qint32 m_quantize_clock, m_quantize_pitch;
  Scale scale;
  LocalEditState(const pxtnService *pxtn, const EditState &s) {
    update(pxtn, s);
  };
  void update(const pxtnService *pxtn, const EditState &s);
};

class KeyboardView : public QWidget {
  Q_OBJECT
 public:
  explicit KeyboardView(PxtoneClient *client, MooClock *moo_clock,
                        QScrollArea *parent = nullptr);
  void cycleCurrentUnit(int offset);

 public slots:
  void toggleTestActivity();
  void selectAll();
  void deselect();
  void transposeSelection(Direction dir, bool wide, bool shift);
  void cutSelection();
  void copySelection();
  void clearSelection();
  void paste();

 private:
  void mousePressEvent(QMouseEvent *event) override;
  void mouseReleaseEvent(QMouseEvent *event) override;
  void mouseMoveEvent(QMouseEvent *event) override;
  void paintEvent(QPaintEvent *event) override;
  void wheelEvent(QWheelEvent *event) override;
  void refreshQuantSettings();
  QSize sizeHint() const override;
  std::set<int> selectedUnitNos();
  const pxtnService *m_pxtn;
  QElapsedTimer *m_timer;
  int painted;
  LocalEditState m_edit_state;
  std::unique_ptr<NotePreview> m_audio_note_preview;
  Animation *m_anim;
  PxtoneClient *m_client;
  MooClock *m_moo_clock;

  bool m_test_activity;
  Clipboard m_clipboard;
};

#endif  // KEYBOARDVIEW_H