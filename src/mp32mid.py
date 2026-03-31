import sys
import os

try:
    from basic_pitch.inference import predict_and_save
except ImportError:
    print("Error: 'basic-pitch' not installed. Please run: pip install basic-pitch")
    sys.exit(1)

def mp3_to_midi(input_path):
    output_directory = "midi"
    if not os.path.exists(output_directory):
        os.makedirs(output_directory)
    
    # Transcription via Spotify's basic-pitch
    predict_and_save(
        audio_path_list=[input_path],
        output_directory=output_directory,
        save_midi=True,
        sonify_midi=False,
        save_model_outputs=False,
        save_notes=False,
    )

if __name__ == "__main__":
    if len(sys.argv) < 2:
        print("Usage: python mp32mid.py <input_audio_file>")
        sys.exit(1)
    mp3_to_midi(sys.argv[1])