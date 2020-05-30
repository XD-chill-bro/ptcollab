#include "KeyboardEditor.h"

#include <QDebug>
#include <QPaintEvent>
#include <QPainter>
#include <QTime>
KeyboardEditor::KeyboardEditor(pxtnService *pxtn, QAudioOutput *audio_output,
                               QWidget *parent)
    : QWidget(parent),
      m_pxtn(pxtn),
      m_timer(new QElapsedTimer),
      painted(0),
      m_audio_output(audio_output),
      m_anim(new Animation(this)) {
  m_audio_output->setNotifyInterval(10);
  qDebug() << m_audio_output->notifyInterval();
  m_anim->setDuration(100);
  m_anim->setStartValue(0);
  m_anim->setEndValue(360);
  m_anim->setEasingCurve(QEasingCurve::Linear);
  m_anim->setLoopCount(-1);
  m_anim->start();

  connect(m_anim, SIGNAL(valueChanged(QVariant)), SLOT(update()));
  // connect(m_audio_output, SIGNAL(notify()), SLOT(update()));
}

struct Interval {
  int start;
  int end;

  bool contains(int x) const { return (start <= x && x < end); }

  int length() const { return end - start; }
};

struct LastEvent {
  int clock;
  int value;

  LastEvent(int value) : clock(0), value(value) {}

  void set(EVERECORD const *e) {
    clock = e->clock;
    value = e->value;
  }
};

struct DrawState {
  LastEvent pitch;
  LastEvent velocity;
  std::optional<Interval> ongoingOnEvent;

  DrawState()
      : pitch(EVENTDEFAULT_KEY),
        velocity(EVENTDEFAULT_VELOCITY),
        ongoingOnEvent(std::nullopt) {}
};

struct KeyBlock {
  int pitch;
  Interval segment;
  Interval onEvent;
};

int clockPerPx = 10;
int pitchPerPx = 32;
int pitchOffset = 38400;
int height = 5;
static qreal pitchToY(qreal pitch) {
  return (pitchOffset - pitch) / pitchPerPx;
}
static qreal pitchOfY(qreal y) { return pitchOffset - y * pitchPerPx; }

static void paintBlock(int pitch, const Interval &segment, QPainter &painter,
                       const QBrush &brush) {
  painter.fillRect(segment.start / clockPerPx, pitchToY(pitch),
                   segment.length() / clockPerPx, height, brush);
}

static void paintVerticalLine(QPainter &painter, QBrush const &brush,
                              int clock) {
  painter.fillRect(clock / clockPerPx, 0, 1, 10000, brush);
}

static int lerp(double r, int a, int b) {
  if (r > 1) r = 1;
  if (r < 0) r = 0;
  return a + r * (b - a);
}
static int clamp(int lo, int x, int hi) {
  if (x < lo) return lo;
  if (x > hi) return hi;
  return x;
}
constexpr int EVENTMAX_VELOCITY = 128;
struct Brush {
  int hue;
  int saturation;
  int muted_brightness;
  int base_brightness;
  int on_brightness;

  Brush(int hue, int saturation = 255, int muted_brightness = 20,
        int base_brightness = 220, int on_brightness = 255)
      : hue(hue),
        saturation(saturation),
        muted_brightness(muted_brightness),
        base_brightness(base_brightness),
        on_brightness(on_brightness) {}

  QBrush toQBrush(int velocity, bool on) {
    int brightness =
        lerp(double(velocity) / EVENTMAX_VELOCITY, muted_brightness,
             on ? on_brightness : base_brightness);
    return QBrush(QColor::fromHsl(hue, saturation, brightness));
  }
};

int pixelsPerVelocity = 3;
int impliedVelocity(MouseEditState state) {
  return clamp(
      EVENTDEFAULT_VELOCITY + (state.current_pitch - state.start_pitch) /
                                  pitchPerPx / pixelsPerVelocity,
      0, EVENTMAX_VELOCITY);
}
// TODO: Make an FPS tracker singleton
static qreal iFps;
void KeyboardEditor::paintEvent(QPaintEvent *) {
  ++painted;
  // if (painted > 10) return;
  QPainter painter(this);

  // Draw FPS
  {
    int interval = 20;
    if (!(painted % interval)) {
      qint64 elapsed = m_timer->nsecsElapsed();
      m_timer->restart();
      iFps = 1E9 / elapsed * interval;
    }
    painter.drawText(rect(), QString("%1 FPS").arg(iFps, 0, 'f', 0));
  }

  // Set up drawing structures that we'll use while iterating through events
  std::vector<DrawState> drawStates;
  std::vector<Brush> brushes;
  for (int i = 0; i < m_pxtn->Unit_Num(); ++i) {
    drawStates.emplace_back();
    brushes.emplace_back((360 * i * 3 / 7) % 360, 255);
  }
  painter.setPen(Qt::blue);
  // TODO: usecs is choppy - it's an upper bound that gets worse with buffer
  // size incrase. for longer songs though and lower end comps we probably do
  // want a bigger buffer. The formula fixes the upper bound issue, but perhaps
  // we can do some smoothing with a linear thing too.
  // int bytes_per_second = 4 /* bytes in sample */ * 44100 /* samples per
  // second */; long usecs = m_audio_output->processedUSecs() -
  // long(m_audio_output->bufferSize()) * 10E5 / bytes_per_second;

  long usecs = m_audio_output->processedUSecs();
  int clock = usecs * m_pxtn->master->get_beat_tempo() *
              m_pxtn->master->get_beat_clock() / 60 / 1000000;
  // clock = m_pxtn->moo_get_now_clock();

  int repeat_clock = m_pxtn->master->get_repeat_meas() *
                     m_pxtn->master->get_beat_num() *
                     m_pxtn->master->get_beat_clock();
  int last_clock = m_pxtn->master->get_beat_clock() *
                   m_pxtn->master->get_play_meas() *
                   m_pxtn->master->get_beat_num();
  if (clock > repeat_clock)
    clock = (clock - repeat_clock) % last_clock + repeat_clock;

  // Draw the note blocks! Upon hitting an event, see if we are able to draw a
  // previous block.
  for (const EVERECORD *e = m_pxtn->evels->get_Records(); e != nullptr;
       e = e->next) {
    int i = e->unit_no;
    switch (e->kind) {
      case EVENTKIND_ON:
        // Draw the last block of the previous on event if there's one to draw.
        // TODO: This 'draw previous note block' is duplicated quite a bit.
        if (drawStates[i].ongoingOnEvent.has_value()) {
          Interval on = drawStates[i].ongoingOnEvent.value();
          int start = std::max(drawStates[i].pitch.clock, on.start);
          int end = std::min(e->clock, on.end);
          Interval interval{start, end};
          QBrush brush = brushes[i].toQBrush(drawStates[i].velocity.value,
                                             on.contains(clock));
          paintBlock(drawStates[i].pitch.value, interval, painter, brush);
          if (start == on.start)
            paintBlock(drawStates[i].pitch.value,
                       {start, start + 2 * clockPerPx}, painter,
                       brushes[i].toQBrush(255, true));
        }
        drawStates[i].ongoingOnEvent.emplace(
            Interval{e->clock, e->value + e->clock});
        break;
      case EVENTKIND_VELOCITY:
        drawStates[i].velocity.set(e);
        break;
      case EVENTKIND_KEY:
        // Maybe draw the previous segment of the current on event.
        if (drawStates[i].ongoingOnEvent.has_value()) {
          Interval on = drawStates[i].ongoingOnEvent.value();
          int start = std::max(drawStates[i].pitch.clock, on.start);
          int end = std::min(e->clock, on.end);
          Interval interval{start, end};
          QBrush brush = brushes[i].toQBrush(drawStates[i].velocity.value,
                                             on.contains(clock));
          paintBlock(drawStates[i].pitch.value, interval, painter, brush);
          if (start == on.start)
            paintBlock(drawStates[i].pitch.value,
                       {start, start + 2 * clockPerPx}, painter,
                       brushes[i].toQBrush(255, true));
          if (e->clock > on.end) drawStates[i].ongoingOnEvent.reset();
        }
        drawStates[i].pitch.set(e);
        break;
      default:
        break;
    }
  }

  // After all the events there might be some blocks that are pending a draw.
  for (uint i = 0; i < drawStates.size(); ++i) {
    if (drawStates[i].ongoingOnEvent.has_value()) {
      Interval on = drawStates[i].ongoingOnEvent.value();
      int start = std::max(drawStates[i].pitch.clock, on.start);
      Interval interval{start, on.end};
      QBrush brush =
          brushes[i].toQBrush(drawStates[i].velocity.value, on.contains(clock));
      paintBlock(drawStates[i].pitch.value, interval, painter, brush);
      if (start == on.start)
        paintBlock(drawStates[i].pitch.value, {start, start + 2 * clockPerPx},
                   painter, brushes[i].toQBrush(255, true));
      drawStates[i].ongoingOnEvent.reset();
    }
  }

  // Draw an ongoing edit
  if (m_mouse_edit_state != nullptr) {
    int velocity = impliedVelocity(*m_mouse_edit_state);
    Interval interval{m_mouse_edit_state->start_clock,
                      m_mouse_edit_state->current_clock};
    paintBlock(m_mouse_edit_state->start_pitch, interval, painter,
               brushes[0].toQBrush(velocity, false));
  }

  // clock = us * 1s/10^6us * 1m/60s * tempo beats/m * beat_clock clock/beats
  paintVerticalLine(painter, QBrush(QColor::fromRgb(255, 255, 255)), clock);
  paintVerticalLine(painter, QBrush(QColor::fromRgb(255, 255, 255, 128)),
                    last_clock);
  paintVerticalLine(painter, QBrush(QColor::fromRgb(255, 255, 255, 128)),
                    m_pxtn->moo_get_end_clock());
}

void KeyboardEditor::mousePressEvent(QMouseEvent *event) {
  int clock = event->localPos().x() * clockPerPx;
  int pitch = int(round(pitchOfY(event->localPos().y())));
  MouseEditState::Type type;
  if (event->modifiers() & Qt::ControlModifier) {
    if (event->button() == Qt::RightButton)
      type = MouseEditState::Type::DeleteNote;
    else
      type = MouseEditState::Type::SetNote;
  } else {
    if (event->button() == Qt::RightButton)
      type = MouseEditState::Type::DeleteOn;
    else
      type = MouseEditState::Type::SetOn;
  }

  m_mouse_edit_state.reset(
      new MouseEditState{type, clock, pitch, clock, pitch});
}

void KeyboardEditor::mouseMoveEvent(QMouseEvent *event) {
  if (m_mouse_edit_state == nullptr) return;
  m_mouse_edit_state->current_pitch =
      int(round(pitchOfY(event->localPos().y())));
  m_mouse_edit_state->current_clock = event->localPos().x() * clockPerPx;
}

void KeyboardEditor::mouseReleaseEvent(QMouseEvent *event) {
  if (m_mouse_edit_state == nullptr) return;

  int start_clock = m_mouse_edit_state->start_clock;
  int end_clock = event->localPos().x() * clockPerPx;
  if (end_clock < start_clock) std::swap(start_clock, end_clock);

  int start_pitch = m_mouse_edit_state->start_pitch;
  // int end_pitch = int(round(pitchOfY(event->localPos().y())));
  int start_measure = start_clock / m_pxtn->master->get_beat_clock() /
                      m_pxtn->master->get_beat_num();
  int end_measure = end_clock / m_pxtn->master->get_beat_clock() /
                    m_pxtn->master->get_beat_num();

  switch (m_mouse_edit_state->type) {
    case MouseEditState::SetOn:
      m_pxtn->evels->Record_Delete(start_clock, end_clock, 0, EVENTKIND_ON);
      m_pxtn->evels->Record_Delete(start_clock, end_clock, 0,
                                   EVENTKIND_VELOCITY);
      m_pxtn->evels->Record_Delete(start_clock, end_clock, 0, EVENTKIND_KEY);
      m_pxtn->evels->Record_Add_i(start_clock, 0, EVENTKIND_ON,
                                  end_clock - start_clock);
      m_pxtn->evels->Record_Add_i(start_clock, 0, EVENTKIND_VELOCITY,
                                  impliedVelocity(*m_mouse_edit_state));
      m_pxtn->evels->Record_Add_i(start_clock, 0, EVENTKIND_KEY, start_pitch);
      if (end_measure >= m_pxtn->master->get_meas_num())
        m_pxtn->master->set_meas_num(end_measure + 1);
      break;
    case MouseEditState::DeleteOn:
      m_pxtn->evels->Record_Delete(start_clock, end_clock, 0, EVENTKIND_ON);
      m_pxtn->evels->Record_Delete(start_clock, end_clock, 0,
                                   EVENTKIND_VELOCITY);
      break;
    case MouseEditState::SetNote:
      m_pxtn->evels->Record_Delete(start_clock, end_clock, 0, EVENTKIND_KEY);
      m_pxtn->evels->Record_Add_i(start_clock, 0, EVENTKIND_KEY, start_pitch);
      if (start_measure >= m_pxtn->master->get_meas_num())
        m_pxtn->master->set_meas_num(start_measure + 1);
      break;
    case MouseEditState::DeleteNote:
      m_pxtn->evels->Record_Delete(start_clock, end_clock, 0, EVENTKIND_KEY);
      break;
  }
  m_mouse_edit_state.reset();
}
