// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "base/timer.h"
#include "media/audio/media_audio_capture.h"
#include "rpl/event_stream.h"
#include "rpl/lifetime.h"
#include "rpl/variable.h"

#include <QtCore/QObject>
#include <QtNetwork/QNetworkAccessManager>

class QNetworkReply;

namespace Ayu::SpeechToText {

enum class State : uchar {
	Idle,
	Recording,
	Transcribing,
};

class Engine final : public QObject {
public:
	explicit Engine(QObject *parent = nullptr);
	~Engine();

	[[nodiscard]] rpl::producer<State> stateValue() const;
	[[nodiscard]] rpl::producer<QString> transcribed() const;
	[[nodiscard]] rpl::producer<QString> errors() const;

	void toggle();
	void cancel();
	[[nodiscard]] bool isRecording() const;
	[[nodiscard]] bool isBusy() const;

private:
	void setState(State state);
	void startRecording();
	void stopRecording(bool transcribe);
	void onCaptureUpdate(const Media::Capture::Update &update);
	void onCaptureError();
	void transcribeAudio(QByteArray audio);
	void onReplyFinished(QNetworkReply *reply);

	rpl::variable<State> _state = State::Idle;
	rpl::event_stream<QString> _transcribed;
	rpl::event_stream<QString> _errors;
	rpl::lifetime _captureLifetime;
	bool _heardSpeech = false;
	base::Timer _silenceTimer;
	base::Timer _maxTimer;
	base::Timer _requestTimer;
	QNetworkAccessManager _manager;
	QNetworkReply *_reply = nullptr;

};

} // namespace Ayu::SpeechToText
