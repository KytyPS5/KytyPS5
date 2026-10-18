#pragma once

#include <QByteArray>
#include <QFutureWatcher>
#include <QObject>
#include <QPromise>
#include <QString>
#include <QThreadPool>

#include <vector>

struct SDL_AudioStream;

class TitleSound final: public QObject {
public:
	explicit TitleSound(QObject* parent = nullptr);
	~TitleSound() override;

	void SetTitle(const QString& base);

private:
	struct Sound {
		std::vector<float> pcm;
		int                channels = 0;
		int                rate     = 0;
	};

	static Sound Decode(const QByteArray& data, const QPromise<Sound>& promise);
	void         Stop();

	QThreadPool           m_decode_pool;
	QFutureWatcher<Sound> m_decode;
	QString               m_base;
	SDL_AudioStream*      m_stream            = nullptr;
	bool                  m_audio_initialized = false;
};
