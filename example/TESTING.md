# Testing ravava_loopback on a Mac (for the tester)

`ravava_loopback` records the microphone, encodes it with Opus (48000 Hz, mono, fullband), decodes it again and plays it
on the speaker. It checks that RavAva records and plays on macOS. Nothing is sent over the network.

1. Unzip the file. Open Terminal in that folder.
2. The file came from the internet, macOS blocks it. Allow it once:

       chmod +x ravava_loopback
       xattr -dr com.apple.quarantine ravava_loopback

   (If macOS still refuses: System Settings > Privacy & Security > "Open Anyway".)
3. **Put headphones on** (otherwise the speaker howls into the microphone).
4. List the devices, then run it:

       ./ravava_loopback --list
       ./ravava_loopback -t 15                 # 15 seconds, default microphone and speaker
       ./ravava_loopback -t 15 --loss 10       # with 10 % simulated packet loss

   The first run asks "Terminal would like to access the microphone": click Allow (if you clicked Don't Allow before:
   System Settings > Privacy & Security > Microphone > Terminal).
5. Speak. You should hear yourself with a small delay (about 0.1 - 0.2 s).

Please send back: the full text printed in Terminal (copy it), whether you heard yourself and whether it was clean,
and the output of

    sw_vers; uname -m; system_profiler SPAudioDataType | head -40

Useful options: `-i N` / `-o N` choose the microphone / speaker from `--list`, `-b 32000` bitrate, `-f 40` frame length,
`--music` music mode, `--help` for the rest.
