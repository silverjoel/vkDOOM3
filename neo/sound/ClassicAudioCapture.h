/*
 * Optional main-thread Classic audio diagnostics. No playback policy lives here.
 * Call-site defaults are evaluated by MSVC at the caller, including inline callers.
 */
#ifndef CLASSIC_AUDIO_CAPTURE_H
#define CLASSIC_AUDIO_CAPTURE_H

void I_ClassicAudioRegisterCommands();
void I_ClassicAudioEvent( const char * event, int before = 0, int after = 0,
	const char * detail = "", const char * file = __builtin_FILE(), int line = __builtin_LINE() );
void I_ClassicAudioFrame( const char * reason );
// Only true immediately after hardware.Update() succeeds and consumes its own
// AL error result. false collects CPU state only; it must not query OpenAL.
void I_ClassicAudioSample( bool cleanContext );

#endif
