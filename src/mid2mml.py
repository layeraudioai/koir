import mido
import sys
import os

def midi_to_mml(midi_file):
    mid = mido.MidiFile(midi_file)
    tempo = 120
    notes = ['C', 'C+', 'D', 'D+', 'E', 'F', 'F+', 'G', 'G+', 'A', 'A+', 'B']

    tracks_mml = []
    
    for i, track in enumerate(mid.tracks):
        track_mml = ""
        is_drums = False
        last_octave = -1
        
        # Use a list to handle lookahead for duration
        msgs = list(track)
        for j, msg in enumerate(msgs):
            if msg.type == 'set_tempo':
                tempo = int(mido.tempo2bpm(msg.tempo))
            
            if not msg.is_meta and msg.type == 'note_on' and msg.velocity > 0:
                if msg.channel == 9: is_drums = True
                
                # Handle Rests
                if msg.time > 0:
                    rest_val = int((msg.time / mid.ticks_per_beat) * 4)
                    if rest_val >= 1: track_mml += f"R{max(1, 16//rest_val) if rest_val < 16 else 64}"
                
                octave = (msg.note // 12) - 1
                if octave != last_octave:
                    track_mml += f"O{octave}"
                    last_octave = octave
                
                # Determine duration by looking at the next events
                duration_ticks = 0
                for k in range(j + 1, len(msgs)):
                    duration_ticks += msgs[k].time
                    if msgs[k].type in ['note_on', 'note_off']: break
                
                # Convert ticks to MML length (1, 2, 4, 8, 16, 32)
                ratio = max(0.0625, duration_ticks / mid.ticks_per_beat)
                mml_len = 4
                if ratio >= 3.5: mml_len = 1
                elif ratio >= 1.5: mml_len = 2
                elif ratio >= 0.75: mml_len = 4
                elif ratio >= 0.35: mml_len = 8
                else: mml_len = 16
                
                track_mml += f"{notes[msg.note % 12]}{mml_len}"
        
        if track_mml:
            header = f"{{Drums}}" if is_drums else f"{{Track {i}}}"
            tracks_mml.append(f"{header}\nT{tempo}{track_mml}")
                
    return "\n".join(tracks_mml)

if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(1)
    infile = sys.argv[1]
    base = os.path.basename(infile)
    name = os.path.splitext(base)[0]
    if not os.path.exists("mml"): os.makedirs("mml")
    outfile = os.path.join("mml", name + ".mml")
    try:
        result = midi_to_mml(infile)
        with open(outfile, "w") as f:
            f.write(result)
        print(f"Generated {outfile}")
    except Exception as e:
        print(f"Error converting MIDI: {e}")
        sys.exit(1)