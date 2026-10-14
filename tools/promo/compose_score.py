#!/usr/bin/env python3
"""Compose the original 120 BPM 'A little closer' electronic cue.

No samples, loops, recordings, pretrained audio models or third-party music.
All oscillators, envelopes, percussion, delays and stereo positioning are authored
here. Deterministic seed makes the final soundtrack exactly reproducible.
"""
from pathlib import Path
import math
import wave
import numpy as np

ROOT=Path(__file__).resolve().parent
RATE=48000
DURATION=58
rng=np.random.default_rng(141040)
mix=np.zeros((RATE*DURATION,2),np.float32)


def hz(note): return 440*2**((note-69)/12)


def add(sound,at,amp=1.,pan=0.):
    offset=round(at*RATE)
    if offset<0:
        sound=sound[-offset:];offset=0
    n=min(len(sound),len(mix)-offset)
    if n<=0:return
    angle=(pan+1)*math.pi/4
    gain=np.array([math.cos(angle),math.sin(angle)],np.float32)*amp
    mix[offset:offset+n]+=sound[:n,None]*gain


def env(t,attack=.008,decay=.25,release=.05):
    return np.minimum(1,t/attack)*np.exp(-t/decay)*np.minimum(1,(t[-1]-t)/release)


def pluck(note,dur=.62,bright=.32):
    t=np.arange(int(RATE*dur),dtype=np.float32)/RATE;f=hz(note)
    s=np.sin(2*np.pi*f*t)+bright*np.sin(2*np.pi*f*2*t+.2)+.11*np.sin(2*np.pi*f*3*t)
    return (s*env(t,.003,.18,.06)*.7).astype(np.float32)


def bell(note,dur=1.7):
    t=np.arange(int(RATE*dur),dtype=np.float32)/RATE;f=hz(note)
    s=np.sin(2*np.pi*f*t+.6*np.sin(2*np.pi*f*2*t)*np.exp(-t*7))
    return (s*env(t,.007,.55,.2)).astype(np.float32)


def bass(note,dur=.45):
    t=np.arange(int(RATE*dur),dtype=np.float32)/RATE;f=hz(note)
    s=np.sin(2*np.pi*f*t)+.17*np.sin(2*np.pi*f*2*t)+.04*np.sin(2*np.pi*f*3*t)
    return (s*env(t,.007,.33,.055)).astype(np.float32)


def pad(notes,dur=2.75):
    t=np.arange(int(RATE*dur),dtype=np.float32)/RATE
    s=np.zeros_like(t)
    for note in notes:
        f=hz(note)
        s+=(np.sin(2*np.pi*f*t)+.35*np.sin(2*np.pi*f*1.0017*t)+.12*np.sin(2*np.pi*f*2*t))/len(notes)
    e=np.minimum(1,t/.12)*np.minimum(1,(dur-t)/.9)
    # Deliberate eighth-note pulse lets the cue breathe around the drums.
    pulse=.68+.32*np.sin(t*2*np.pi*2-math.pi/2)**2
    return (s*e*pulse).astype(np.float32)


def kick():
    dur=.32;t=np.arange(int(RATE*dur),dtype=np.float32)/RATE
    f=46+105*np.exp(-t*35)
    phase=2*np.pi*np.cumsum(f)/RATE
    return (np.sin(phase)*np.exp(-t*14)+rng.normal(0,.035,len(t))*np.exp(-t*160)).astype(np.float32)


def snare():
    dur=.23;t=np.arange(int(RATE*dur),dtype=np.float32)/RATE
    noise=rng.normal(0,1,len(t)).astype(np.float32)
    body=.25*np.sin(2*np.pi*183*t)*np.exp(-t*24)
    high=noise-np.convolve(noise,np.ones(7)/7,'same')
    return ((high*.65+body)*env(t,.001,.052,.025)).astype(np.float32)


def hat(opened=False):
    dur=.19 if opened else .065;t=np.arange(int(RATE*dur),dtype=np.float32)/RATE
    n=rng.normal(0,1,len(t)).astype(np.float32)
    n=n-np.convolve(n,np.ones(4)/4,'same')
    return (n*env(t,.001,.048 if opened else .019,.007)).astype(np.float32)


def sweep(at,dur=.6,up=True,amp=.065):
    t=np.arange(int(RATE*dur),dtype=np.float32)/RATE
    n=rng.normal(0,1,len(t)).astype(np.float32)
    n=np.convolve(n,np.ones(6)/6,'same')
    e=np.sin(np.pi*t/dur)**2
    f=(500+1700*t/dur) if up else (2100-1650*t/dur)
    s=(n*.35+np.sin(2*np.pi*np.cumsum(f)/RATE)*.13)*e
    add(s,at,amp,-.65);add(s,at+.045,amp*.8,.65)


# D minor / B-flat major / F / C; warm extensions keep the small synth palette open.
chords=[([62,65,69,76],38),([58,62,65,72],34),([60,65,69,74],41),([60,64,67,74],36)]
for bar in range(29):
    at=bar*2
    notes,root=chords[(bar//2)%4]
    strength=.10 if at<2 else .12
    if at>=50: strength=.10
    add(pad(notes),at,strength,-.17)
    add(pad([n+12 for n in notes],2.5),at+.032,strength*.35,.42)
    if at<2 or at>=54:continue
    sparse=30<=at<36 or 50<=at<54
    for beat in range(4):
        pos=at+beat*.5
        add(bass(root if beat!=3 else root+12),pos,.205 if not sparse else .15)
        if beat in (0,2):add(kick(),pos,.47 if not sparse else .32)
        if beat in (1,3) and not sparse:add(snare(),pos,.125,.08)
    if not sparse:
        for eighth in range(8):
            add(hat(opened=(eighth==7)),at+eighth*.25+.012,.041 if eighth%2 else .028,(-1 if eighth%2 else 1)*.43)
    # The arpeggio is sparse enough to leave space for motion chirps.
    if not (22<=at<26):
        pattern=[0,2,1,3,2,0,3,1]
        for step,index in enumerate(pattern):
            if sparse and step%2:continue
            n=notes[index]+12
            s=pluck(n,bright=.18 if at>=36 else .28)
            level=.055 if at<14 else .043
            pos=at+step*.25
            add(s,pos,level,math.sin(step*.9)*.52)
            add(s,pos+.375,level*.28,-math.sin(step*.9)*.52)

# A recognizable five-note motif bookends the film, plus a restrained bridge.
for at,trans in [(2,0),(10,-2),(18,0),(38,0),(44,0),(50,0)]:
    motif=[(0,81),(.5,86),(1.25,89),(2,88),(2.75,86)]
    for index,(offset,note) in enumerate(motif):
        s=bell(note+trans)
        add(s,at+offset,.058 if at<50 else .075,-.22+index*.11)
        add(s,at+offset+.5,.018,.6-index*.2)

# Designed three-tone acoustic motif, unrelated to the actual invitation encoding.
for at in [6.5,8.5,10.5]:
    for i,n in enumerate([86,93,90]):
        add(pluck(n,.19,.10),at+i*.115,.071,-.48+i*.4)
for at in [5.65,13.65,21.65,29.65,35.65,43.65,49.65]:
    sweep(at,.7,amp=.11)
# Ending resolves to a spacious D minor ninth and fades, leaving the URL time to read.
add(pad([50,57,62,65,69,76],4.0),54,.18)
add(bell(86,3.0),54,.095,-.15)
add(bell(81,3.0),54.02,.06,.2)

# Original short reflections: zero external reverberation or sample dependencies.
for delay,level in [(.083,.05),(.149,.033)]:
    n=round(delay*RATE)
    mix[n:]+=mix[:-n,::-1]*level
# Gentle bus saturation and a transparent peak ceiling.
mix=np.tanh(mix*1.18)/1.18
peak=float(np.max(np.abs(mix)))
mix*=min(10**(4.67/20),.84/max(peak,1e-8))
fade=np.ones(len(mix),np.float32)
fade[:int(RATE*.28)]=np.linspace(0,1,int(RATE*.28))
fade[-int(RATE*2):]=np.linspace(1,0,int(RATE*2))**1.4
mix*=fade[:,None]
peak=float(np.max(np.abs(mix)));rms=float(np.sqrt(np.mean(mix**2)))
out=ROOT/'a-little-closer-original-score.wav'
with wave.open(str(out),'wb') as f:
    f.setnchannels(2);f.setsampwidth(2);f.setframerate(RATE)
    f.writeframes((mix*32767).astype('<i2').tobytes())
print(f'{out}\nDuration: {DURATION}s / stereo {RATE} Hz / peak {20*math.log10(peak):.2f} dBFS / RMS {20*math.log10(rms):.2f} dBFS')
