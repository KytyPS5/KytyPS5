#pragma once

#include <QByteArray>
#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QPromise>
#include <QString>
#include <QThreadPool>
#include <QTimer>

#include <memory>
#include <vector>

struct SDL_AudioStream;

// Plays the selected game's snd0.at9 in a loop. Switching games crossfades: the old track keeps
// playing while it fades out and the new one fades in on its own audio stream.
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

	struct Voice {
		QString                       base;
		std::shared_ptr<const Sound>  sound;
		SDL_AudioStream*              stream = nullptr;
		float                         gain   = 0.0f;
		float                         target = 1.0f;
	};

	static Sound Decode(const QByteArray& data, const QPromise<Sound>& promise);
	void         AddVoice(const QString& base, std::shared_ptr<const Sound> sound);
	void         Tick();
	void         Clear();

	QThreadPool                                      m_decode_pool;
	QFutureWatcher<Sound>                            m_decode;
	QTimer                                           m_timer;
	QHash<QString, std::shared_ptr<const Sound>>     m_cache;
	std::vector<Voice>                               m_voices;
	QString                                          m_base;
	bool                                             m_audio_initialized = false;
	SDL_AudioStream*                                 m_keepalive         = nullptr;

	bool EnsureAudio();
};
