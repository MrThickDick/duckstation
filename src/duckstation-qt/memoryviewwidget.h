#pragma once
#include <QtWidgets/QAbstractScrollArea>

// Based on https://stackoverflow.com/questions/46375673/how-can-realize-my-own-memory-viewer-by-qt

class MemoryViewWidget : public QAbstractScrollArea
{
public:
  Q_OBJECT
public:
  MemoryViewWidget(QWidget* parent = nullptr, size_t address_offset = 0, const void* data_ptr = nullptr, size_t data_size = 0);
  ~MemoryViewWidget();

  void setData(size_t address_offset, const void* data_ptr, size_t data_size);
  void scrollToAddress(size_t address);
  void setFont(const QFont& font);

protected:
  void paintEvent(QPaintEvent*);
  void resizeEvent(QResizeEvent*);

private Q_SLOTS:
  void adjustContent();

private:
  int addressWidth();
  int hexWidth();
  int asciiWidth();
  void updateMetrics();

  const void* m_data;
  size_t m_data_size;
  size_t m_address_offset;

  unsigned nBlockAddress;
  unsigned mBytesPerLine;

  int pxWidth;
  int pxHeight;

  size_t startPos;
  size_t endPos;

  int nRowsVisible;
};