// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/features/speech_to_text/ayu_speech_to_text.h"

#include "ayu/ayu_settings.h"
#include "lang/lang_keys.h"

#include <QtCore/QJsonArray.h>
#include <QtCore/QJsonDocument.h>
#include <QtCore/QJsonObject.h>
#include <QtNetwork/QNetworkReply.h>
#include <QtNetwork/QNetworkRequest.h>

namespace Ayu::SpeechToText {
namespace {

constexpr auto kSilenceTimeout = crl::time(3000);
constexpr auto kNoSpeechTimeout = crl::time(8000);
constexpr auto kMaxDuration = crl::time(120000);
constexpr auto kRequestTimeout = crl::time(60000);
constexpr auto kSpeechLevel = 1000;
const auto kApiRoot = u"https://generativelanguage.googleapis.com/v1beta"_q;
const auto kPrompt = u"Transcribe the speech in this audio recording exactly, in the language that is spoken. Reply with a JSON object {\"speech\": true/false, \"text\": \"...\"}: set speech to false and text to an empty string if there is no speech in the audio. Output only that JSON object."_q;

QString errorMessage(const QByteArray &body) {
	const auto document = QJsonDocument::fromJson(body);
	return document.object()
		.value(u"error"_q).toObject()
		.value(u"message"_q).toString();
}

QString errorForStatus(int status, const QString &message) {
	if (status == 400) {
		return message.contains(u"api key"_q, Qt::CaseInsensitive)
			? tr::ayu_SpeechToTextInvalidKey(tr::now)
			: tr::ayu_SpeechToTextFailed(tr::now);
	} else if (status == 404) {
		return tr::ayu_SpeechToTextModelMissing(tr::now);
	} else if (status == 429) {
		return tr::ayu_SpeechToTextQuota(tr::now);
	}
	return tr::ayu_SpeechToTextFailed(tr::now);
}

QString parseTranscript(const QByteArray &body, bool &noSpeech) {
	noSpeech = false;
	const auto document = QJsonDocument::fromJson(body);
	const auto candidates = document.object()
		.value(u"candidates"_q).toArray();
	if (candidates.isEmpty()) {
		return QString();
	}
	const auto parts = candidates.at(0).toObject()
		.value(u"content"_q).toObject()
		.value(u"parts"_q).toArray();
	auto raw = QString();
	for (const auto &part : parts) {
		raw += part.toObject().value(u"text"_q).toString();
	}
	raw = raw.trimmed();
	if (raw.isEmpty()) {
		return QString();
	}
	if (raw.startsWith(u"```"_q)) {
		const auto firstLine = raw.indexOf('\n');
		raw = (firstLine >= 0) ? raw.mid(firstLine + 1) : QString();
		if (raw.endsWith(u"```"_q)) {
			raw.chop(3);
		}
		raw = raw.trimmed();
	}
	const auto answer = QJsonDocument::fromJson(raw.toUtf8());
	if (answer.isObject()) {
		const auto object = answer.object();
		if (object.contains(u"speech"_q) || object.contains(u"text"_q)) {
			if (!object.value(u"speech"_q).toBool(true)) {
				noSpeech = true;
				return QString();
			}
			return object.value(u"text"_q).toString().trimmed();
		}
	}
	return raw;
}

QJsonObject safetySetting(const QString &category) {
	return QJsonObject{
		{ u"category"_q, category },
		{ u"threshold"_q, u"BLOCK_ONLY_HIGH"_q },
	};
}

} // namespace

Engine::Engine(QObject *parent)
: QObject(parent) {
	_silenceTimer.setCallback([=] { stopRecording(true); });
	_maxTimer.setCallback([=] { stopRecording(true); });
	_requestTimer.setCallback([=] {
		if (_reply) {
			_reply->abort();
		}
	});
}

Engine::~Engine() {
	cancel();
}

rpl::producer<State> Engine::stateValue() const {
	return _state.value();
}

rpl::producer<QString> Engine::transcribed() const {
	return _transcribed.events();
}

rpl::producer<QString> Engine::errors() const {
	return _errors.events();
}

void Engine::toggle() {
	if (_state.current() == State::Recording) {
		stopRecording(true);
	} else if (_state.current() == State::Idle) {
		startRecording();
	}
}

void Engine::cancel() {
	_silenceTimer.cancel();
	_maxTimer.cancel();
	_requestTimer.cancel();
	_captureLifetime.destroy();
	if (_reply) {
		const auto reply = base::take(_reply);
		reply->abort();
	}
	if (_state.current() == State::Recording) {
		::Media::Capture::instance()->stop();
	}
	setState(State::Idle);
}

bool Engine::isRecording() const {
	return _state.current() == State::Recording;
}

bool Engine::isBusy() const {
	return _state.current() != State::Idle;
}

void Engine::setState(State state) {
	if (_state.current() == state) {
		return;
	}
	_state = state;
}

void Engine::startRecording() {
	if (::Media::Capture::instance()->started()) {
		_errors.fire(tr::ayu_SpeechToTextBusy(tr::now));
		return;
	}
	_heardSpeech = false;
	::Media::Capture::instance()->start();
	::Media::Capture::instance()->updated(
	) | rpl::on_next_error([=](const Media::Capture::Update &update) {
		onCaptureUpdate(update);
	}, [=] {
		onCaptureError();
	}, _captureLifetime);
	_silenceTimer.callOnce(kNoSpeechTimeout);
	_maxTimer.callOnce(kMaxDuration);
	setState(State::Recording);
}

void Engine::stopRecording(bool transcribe) {
	_silenceTimer.cancel();
	_maxTimer.cancel();
	_captureLifetime.destroy();
	if (_state.current() != State::Recording) {
		return;
	}
	setState(transcribe ? State::Transcribing : State::Idle);
	if (transcribe) {
		::Media::Capture::instance()->stop(
			crl::guard(this, [=](Media::Capture::Result &&data) {
				if (!_heardSpeech || data.bytes.isEmpty()) {
					setState(State::Idle);
					if (!_heardSpeech) {
						_errors.fire(tr::ayu_SpeechToTextNoSpeech(tr::now));
					}
					return;
				}
				transcribeAudio(std::move(data.bytes));
			}));
	} else {
		::Media::Capture::instance()->stop();
	}
}

void Engine::onCaptureUpdate(const Media::Capture::Update &update) {
	if (update.level >= kSpeechLevel) {
		_heardSpeech = true;
		_silenceTimer.callOnce(kSilenceTimeout);
	}
}

void Engine::onCaptureError() {
	_silenceTimer.cancel();
	_maxTimer.cancel();
	_captureLifetime.destroy();
	if (_state.current() == State::Recording) {
		::Media::Capture::instance()->stop();
	}
	setState(State::Idle);
	_errors.fire(tr::ayu_SpeechToTextMicError(tr::now));
}

void Engine::transcribeAudio(QByteArray audio) {
	const auto &settings = AyuSettings::getInstance();
	const auto key = settings.geminiApiKey();
	const auto model = settings.geminiModel();
	if (key.isEmpty()) {
		setState(State::Idle);
		_errors.fire(tr::ayu_SpeechToTextNoKey(tr::now));
		return;
	}
	const auto body = QJsonObject{
		{ u"systemInstruction"_q, QJsonObject{
			{ u"parts"_q, QJsonArray{ QJsonObject{
				{ u"text"_q, kPrompt },
			} } },
		} },
		{ u"contents"_q, QJsonArray{ QJsonObject{
			{ u"role"_q, u"user"_q },
			{ u"parts"_q, QJsonArray{
				QJsonObject{
					{ u"text"_q, u"Transcribe the audio."_q },
				},
				QJsonObject{
					{ u"inlineData"_q, QJsonObject{
						{ u"mimeType"_q, u"audio/ogg"_q },
						{ u"data"_q, QString::fromLatin1(audio.toBase64()) },
					} },
				},
			} },
		} } },
		{ u"safetySettings"_q, QJsonArray{
			safetySetting(u"HARM_CATEGORY_HARASSMENT"_q),
			safetySetting(u"HARM_CATEGORY_HATE_SPEECH"_q),
			safetySetting(u"HARM_CATEGORY_SEXUALLY_EXPLICIT"_q),
			safetySetting(u"HARM_CATEGORY_DANGEROUS_CONTENT"_q),
		} },
		{ u"generationConfig"_q, QJsonObject{
			{ u"temperature"_q, 0 },
			{ u"maxOutputTokens"_q, 8192 },
			{ u"responseMimeType"_q, u"application/json"_q },
			{ u"responseSchema"_q, QJsonObject{
				{ u"type"_q, u"OBJECT"_q },
				{ u"properties"_q, QJsonObject{
					{ u"speech"_q, QJsonObject{
						{ u"type"_q, u"BOOLEAN"_q },
					} },
					{ u"text"_q, QJsonObject{
						{ u"type"_q, u"STRING"_q },
					} },
				} },
				{ u"required"_q, QJsonArray{ u"speech"_q, u"text"_q } },
				{ u"propertyOrdering"_q, QJsonArray{ u"speech"_q, u"text"_q } },
			} },
		} },
	};
	auto request = QNetworkRequest(QUrl(
		u"%1/models/%2:generateContent"_q.arg(kApiRoot, model)));
	request.setHeader(
		QNetworkRequest::ContentTypeHeader,
		u"application/json"_q);
	request.setRawHeader("x-goog-api-key", key.toUtf8());
	_reply = _manager.post(
		request,
		QJsonDocument(body).toJson(QJsonDocument::Compact));
	connect(_reply, &QNetworkReply::finished, this, [=] {
		onReplyFinished(_reply);
	});
	_requestTimer.callOnce(kRequestTimeout);
}

void Engine::onReplyFinished(QNetworkReply *reply) {
	_requestTimer.cancel();
	if (reply != _reply) {
		reply->deleteLater();
		return;
	}
	_reply = nullptr;
	const auto status = reply->attribute(
		QNetworkRequest::HttpStatusCodeAttribute).toInt();
	const auto body = reply->readAll();
	const auto networkError = reply->error();
	reply->deleteLater();
	if (_state.current() != State::Transcribing) {
		return;
	}
	if (networkError != QNetworkReply::NoError) {
		setState(State::Idle);
		_errors.fire(status
			? errorForStatus(status, errorMessage(body))
			: tr::ayu_SpeechToTextOffline(tr::now));
		return;
	}
	auto noSpeech = false;
	const auto text = parseTranscript(body, noSpeech);
	setState(State::Idle);
	if (noSpeech) {
		_errors.fire(tr::ayu_SpeechToTextNoSpeech(tr::now));
	} else if (text.isEmpty()) {
		_errors.fire(tr::ayu_SpeechToTextFailed(tr::now));
	} else {
		_transcribed.fire(text);
	}
}

} // namespace Ayu::SpeechToText
