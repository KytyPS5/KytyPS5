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

	// One-shot navigation click, played over the title music.
	void PlayNavigation();

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
		size_t                        pos    = 0;
	};

	static Sound Decode(const QByteArray& data, const QPromise<Sound>& promise);
	static void  Feed(Voice& v);
	void         AddVoice(const QString& base, std::shared_ptr<const Sound> sound);
	void         FadeOthers(const QString& keep);
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
	std::vector<SDL_AudioStream*>                    m_nav_streams;
	QByteArray                                       m_nav_pcm;
	int                                              m_nav_channels = 0;
	int                                              m_nav_rate     = 0;
	bool                                             m_nav_loaded   = false;

	bool EnsureAudio();
};
