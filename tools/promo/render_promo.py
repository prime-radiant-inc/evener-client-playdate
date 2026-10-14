#!/usr/bin/env python3
"""Render an original Evener for Playdate promotional film from real captures.

The frame renderer is intentionally self-contained: Pillow + NumPy + FFmpeg.
Display capture pixels are preserved; graphic housing and motifs are illustrations.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import subprocess
import time
from functools import lru_cache

import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

ROOT = Path(__file__).resolve().parent
ASSETS = ROOT / 'assets'
W, H = 1920, 1080
DURATION = 58.0
FPS = 60
INK = (21, 25, 25)
YELLOW = (255, 211, 42)
PAPER = (244, 242, 233)
MUTED = (139, 146, 141)
WHITE = (255, 255, 253)
DISPLAY_RECT = (50, 49, 440, 264)
A_BUTTON_CENTER = (469, 421)
FONT = '/System/Library/Fonts/Avenir Next.ttc'
SCENES = [(0,6,'intro'),(6,14,'setup'),(14,22,'read'),(22,30,'voice'),
          (30,36,'review'),(36,44,'native'),(44,50,'hero'),(50,58,'end')]


def clamp(x, a=0., b=1.):
    return min(b, max(a, x))


def ease(x):
    x = clamp(x)
    return 1 - (1-x)**4


def smooth(x):
    x = clamp(x)
    return x*x*(3-2*x)


def mix(a,b,t):
    return a + (b-a)*t


def color_mix(a,b,t):
    return tuple(round(mix(x,y,t)) for x,y in zip(a,b))


@lru_cache(128)
def font(size, weight='bold'):
    indices = {'bold':0, 'demi':2, 'medium':5, 'regular':7, 'heavy':8}
    return ImageFont.truetype(FONT, int(size), index=indices[weight])


def text(im, value, x, y, size=80, fill=INK, weight='bold', alpha=1.0,
         tracking=None, anchor='lt'):
    if alpha <= 0:
        return
    f = font(size, weight)
    layer = im if alpha>=.999 else Image.new('RGBA',im.size)
    d = ImageDraw.Draw(layer)
    rgba = tuple(fill[:3]) + (round(255*clamp(alpha)),) if layer.mode=='RGBA' else fill
    if tracking is None:
        d.text((round(x),round(y)),value,font=f,fill=rgba,anchor=anchor)
    else:
        for c in value:
            d.text((round(x),round(y)-f.getbbox('H',anchor='la')[1]),c,font=f,fill=rgba,anchor='la')
            x += d.textlength(c,font=f)+tracking
    if layer is not im:
        im.alpha_composite(layer)


def kinetic_line(im, value, x, y, size, fill, t, delay=0, weight='bold'):
    p=ease((t-delay)/.75)
    text(im,value,x,y+80*(1-p),size,fill,weight,alpha=p)


def lines(im, values, x,y,size,fill,t, gap=None,delay=0,weight='bold'):
    gap=gap or size*1.04
    for i,value in enumerate(values):
        kinetic_line(im,value,x,y+i*gap,size,fill,t,delay+i*.14,weight)


def capsule(im, value,x,y,w=None,fill=INK,foreground=PAPER,size=24,outline=None):
    f=font(size,'demi')
    width=w or ImageDraw.Draw(im).textlength(value,font=f)+44
    d=ImageDraw.Draw(im)
    d.rounded_rectangle((x,y,x+width,y+54),radius=27,fill=fill,outline=outline,width=2)
    text(im,value,x+22,y+13,size,foreground,'demi')
    return width


@lru_cache(12)
def background(style):
    base={'ink':INK,'yellow':YELLOW,'paper':PAPER}[style]
    # A very quiet radial light gives moving objects room without visual noise.
    xx=np.linspace(-1,1,W,dtype=np.float32)[None,:]
    yy=np.linspace(-1,1,H,dtype=np.float32)[:,None]
    glow=np.maximum(0,1-np.sqrt((xx-.3)**2+(yy+.15)**2)/1.65)
    amt=7 if style=='ink' else 5
    arr=np.empty((H,W,3),dtype=np.uint8)
    for c in range(3):
        arr[:,:,c]=np.clip(base[c]+glow*amt,0,255).astype(np.uint8)
    im=Image.fromarray(arr).convert('RGBA')
    if style=='paper':
        d=ImageDraw.Draw(im)
        for x in range(34,W,36):
            for y in range(34,H,36):
                d.ellipse((x,y,x+1,y+1),fill=(211,213,202,110))
    return im


def rounded_line(draw,pts,fill,width):
    draw.line(pts,fill=fill,width=width,joint='curve')
    r=width/2
    for x,y in [pts[0],pts[-1]]:
        draw.ellipse((x-r,y-r,x+r,y+r),fill=fill)


@lru_cache(1)
def official_logo():
    return Image.open(ASSETS/'primeradiant-logo.png').convert('RGBA')


@lru_cache(16)
def logo_at(width):
    im=official_logo()
    return im.resize((width,round(im.height*width/im.width)),Image.Resampling.LANCZOS)


def logo_bug(im,t,scene_no,preview=False):
    d=ImageDraw.Draw(im)
    d.rounded_rectangle((W-327,48,W-60,130),radius=18,fill=INK)
    mark=logo_at(221)
    im.alpha_composite(mark,(W-304,59))
    # This indexing line belongs to the film, not to the product interface.
    text(im,'EVENER / PLAYDATE',70,63,22,
         PAPER if scene_no in (0,3,5,7) else INK,'demi',tracking=2.8)
    if scene_no != 7:
        dark=scene_no in (0,3,5)
        if dark:
            col=(87,92,88)
        elif scene_no in (1,4,6):
            col=(193,166,64)
        else:
            col=(198,202,194)
        d.line((70,H-48,W-70,H-48),fill=col,width=2)
        d.line((70,H-48,70+(W-140)*clamp(t/DURATION),H-48),fill=YELLOW if dark else INK,width=3)
        text(im,f'{scene_no+1:02d} / 08',W-174,H-89,18,PAPER if dark else INK,'medium',tracking=1.2)
    if preview:
        d.rounded_rectangle((620,1000,1320,1048),radius=10,fill=(137,36,32,245))
        text(im,'STYLE PREVIEW • SCREEN CAPTURE PLACEHOLDER',646,1013,18,WHITE,'bold',tracking=.3)


class Footage:
    """Sequential raw-video readers keep peak memory independent of clip length."""
    def __init__(self,path,preview=False):
        self.manifest_path=Path(path).resolve()
        self.manifest=json.loads(self.manifest_path.read_text())
        self.preview=self.manifest.get('preview',False) or preview
        self.inputs=self.manifest['screens']
        self.readers={}
        self.images={}
        self.transition_stills={}
        for key in ['setup','read','voice','review','hero']:
            if key not in self.inputs:
                raise ValueError(f'Missing real screen input: {key}')
            p=Path(self.inputs[key]['path'])
            if not p.is_absolute():p=self.manifest_path.parent/p
            self.inputs[key]['path']=str(p)
            if not p.exists():
                raise ValueError(f'Missing screen asset: {p}')
            if not self.preview and p.suffix.lower() in ('.png','.jpg','.jpeg'):
                raise ValueError('Final film requires actual screen video for every screen sequence.')
            if p.suffix.lower() not in ('.png','.jpg','.jpeg'):
                info=json.loads(subprocess.check_output(['ffprobe','-v','error','-select_streams','v:0',
                    '-show_entries','stream=width,height','-of','json',str(p)]))['streams'][0]
                crop=self.inputs[key].get('crop')
                width,height=crop[2:] if crop else (info['width'],info['height'])
                if abs(width/height-400/240)>.005:
                    raise ValueError(f'{key}: screen source must have a 400:240 aspect ratio; specify its exact display crop.')

    def close(self):
        for v in self.readers.values():
            v['proc'].terminate()
        self.readers.clear()

    def frame(self,key,t):
        cfg=self.inputs[key]
        path=cfg['path']
        if Path(path).suffix.lower() in ('.png','.jpg','.jpeg'):
            if key not in self.images:
                self.images[key]=Image.open(path).convert('RGB').resize((400,240),Image.Resampling.NEAREST)
            return self.images[key]
        # A trim is explicitly an editorial trim, never a simulated UI action.
        rate=float(cfg.get('speed',1))
        target=max(0,int(t*rate*FPS))
        v=self.readers.get(key)
        if v is None or target<v['index']:
            if v is not None:
                v['proc'].terminate()
            filt=[]
            if cfg.get('crop'):
                x,y,w,h=cfg['crop']
                filt.append(f'crop={w}:{h}:{x}:{y}')
            # Normalize held VFR frames before trimming; input seeking can skip the
            # frame that owns a long unchanged display interval.
            filt.extend(['format=rgb24','scale=400:240:flags=neighbor',f'fps={FPS}',
                         f"trim=start={cfg.get('in',0)}",'setpts=PTS-STARTPTS'])
            cmd=['ffmpeg','-hide_banner','-loglevel','error','-i',path,'-an',
                 '-vf',','.join(filt),'-f','rawvideo','-pix_fmt','rgb24','pipe:1']
            p=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=subprocess.DEVNULL)
            v={'proc':p,'index':-1,'frame':None}
            self.readers[key]=v
        while v['index']<target:
            raw=v['proc'].stdout.read(400*240*3)
            if len(raw)!=400*240*3:
                if v['frame'] is None:
                    raise ValueError(f'No decoded screen frame: {path}')
                break
            v['frame']=Image.frombytes('RGB',(400,240),raw)
            v['index']+=1
        return v['frame']


class ScaledDraw:
    """Draw the housing at double resolution before its final camera transform."""
    def __init__(self, image, scale=2):
        self.draw=ImageDraw.Draw(image)
        self.scale=scale

    def coordinates(self,value):
        if isinstance(value,(int,float)):
            return round(value*self.scale)
        return [self.coordinates(v) for v in value]

    def __getattr__(self,name):
        def invoke(coords,*args,**kwargs):
            for key in ('width','radius'):
                if key in kwargs:kwargs[key]=round(kwargs[key]*self.scale)
            return getattr(self.draw,name)(self.coordinates(coords),*args,**kwargs)
        return invoke


@lru_cache(1)
def housing():
    """Graphic front housing, proportioned from the supplied Playdate reference."""
    im=Image.new('RGBA',(1440,1400))
    d=ScaledDraw(im)
    # The almost-square front has a shallow side wall and an asymmetric bezel.
    d.rounded_rectangle((24,17,584,605),radius=30,fill=(158,117,10))
    d.rounded_rectangle((21,13,581,601),radius=30,fill=(224,173,16))
    d.rounded_rectangle((17,10,577,598),radius=29,fill=YELLOW)
    d.rounded_rectangle((20,13,574,595),radius=27,outline=(255,232,112),width=2)
    d.rounded_rectangle((24,17,516,337),radius=25,fill=(42,45,42))
    d.rounded_rectangle((47,46,493,316),radius=2,fill=(12,17,14))
    # Menu button and speaker occupy the narrow strip beside the display.
    d.ellipse((524,77,568,120),fill=(177,130,16))
    d.ellipse((525,76,567,118),fill=(255,217,70),outline=(195,151,29),width=2)
    d.ellipse((538,89,554,105),fill=WHITE)
    d.rounded_rectangle((535,142,557,238),radius=11,fill=(127,135,129))
    for row in range(14):
        for col in range(3):
            x=540+col*6;y=148+row*6.3+(3 if col==1 else 0)
            d.ellipse((x-1.8,y-1.8,x+1.8,y+1.8),fill=(38,44,41))
    # Rounded cross and level B/A controls follow the actual front layout.
    for offset,fill,inset in [(3,(170,127,16),0),(0,(191,148,31),0),
                              (0,(255,220,79),3)]:
        d.rounded_rectangle((127+inset,385+inset+offset,175-inset,537-inset+offset),
                            radius=24-inset,fill=fill)
        d.rounded_rectangle((75+inset,437+inset+offset,227-inset,485-inset+offset),
                            radius=24-inset,fill=fill)
    for x,y,label in [(357,421,'B'),(*A_BUTTON_CENTER,'A')]:
        d.ellipse((x-33,y-30,x+33,y+36),fill=(177,130,16))
        d.ellipse((x-32,y-32,x+32,y+32),fill=(255,217,65),outline=(190,146,28),width=2)
        d.ellipse((x-17,y-17,x+17,y+17),fill=WHITE)
        text(im,label,x*2,y*2,51,YELLOW,'demi',anchor='mm')
    # Three exposed metal fasteners: top right and the two lower corners.
    for x,y in [(546,42),(47,567),(547,567)]:
        d.ellipse((x-22,y-22,x+22,y+22),fill=(62,68,64))
        d.ellipse((x-20,y-20,x+20,y+20),fill=(183,190,185))
        d.arc((x-17,y-17,x+17,y+17),155,300,fill=(224,228,222),width=3)
        d.ellipse((x-9,y-9,x+9,y+9),fill=(93,101,95))
        d.ellipse((x-7,y-7,x+7,y+7),fill=(21,27,24))
    return im


@lru_cache(8)
def device_shadow(width, height):
    shadow=Image.new('RGBA',(width+100,height+100))
    ImageDraw.Draw(shadow).ellipse((50,50,width+50,height+50),fill=(0,0,0,43))
    return shadow.filter(ImageFilter.GaussianBlur(22))


def device(im,screen,cx,cy,scale=1.,angle=0.,crank=0.,hold=0.,shadow=True):
    body=housing().copy()
    # Source pixels enter only here, unretouched and at their native aspect ratio.
    sx,sy,sw,sh=DISPLAY_RECT
    src=screen.resize((sw*2,sh*2),Image.Resampling.NEAREST)
    body.paste(src,(sx*2,sy*2))
    d=ScaledDraw(body)
    # The crank's axle leaves the right side at control height. Its narrow
    # projected orbit stays outside the face as the arm turns in depth.
    d.ellipse((575,441,595,469),fill=(185,137,16))
    d.ellipse((577,443,593,467),fill=(248,197,35))
    rounded_line(d,[(586,455),(606,455)],(126,135,130),17)
    gx=606+16*math.cos(crank)
    gy=455-149*math.sin(crank)
    rounded_line(d,[(608,458),(gx+2,gy+3)],(91,100,95),24)
    rounded_line(d,[(606,455),(gx,gy)],(192,201,195),22)
    rounded_line(d,[(602,453),(gx-4,gy-2)],(232,236,227),4)
    d.ellipse((gx-12,gy-12,gx+12,gy+12),fill=(184,193,187))
    d.ellipse((gx-7,gy-7,gx+7,gy+7),fill=YELLOW,outline=(165,127,26),width=1)
    if hold>0:
        ax,ay=A_BUTTON_CENTER
        rad=43+5*math.sin(hold*4)
        d.ellipse((ax-rad,ay-rad,ax+rad,ay+rad),outline=(255,255,230),width=4)
        d.ellipse((ax-36,ay-36,ax+36,ay+36),outline=(255,248,180),width=3)
    width=max(1,round(720*scale));height=max(1,round(700*scale))
    body=body.resize((width,height),Image.Resampling.LANCZOS)
    # Reinsert screen after the housing resize to keep its monochrome pixel edges.
    exact=screen.resize((round(sw*scale),round(sh*scale)),Image.Resampling.NEAREST)
    body.paste(exact,(round(sx*scale),round(sy*scale)))
    if abs(angle)>.05:
        body=body.rotate(angle,resample=Image.Resampling.BICUBIC,expand=True)
    x=round(cx-body.width/2);y=round(cy-body.height/2)
    if shadow:
        # A soft elliptical grounding shadow is independent of the device motion.
        sw=round(580*scale);sh=round(76*scale)
        im.alpha_composite(device_shadow(sw,sh),(round(cx-sw/2-40),round(cy+290*scale)))
    im.alpha_composite(body,(x,y))


def circles(im,cx,cy,t,dark=True,maxr=880):
    d=ImageDraw.Draw(im)
    for i in range(7):
        r=110+i*112+(t*33)%112
        if r>maxr: continue
        col=(62,62,35,150) if dark else (209,161,22,160)
        d.ellipse((cx-r,cy-r,cx+r,cy+r),outline=col,width=2)


def wave(im,t,x=84,y=785,width=1710,height=74,fill=YELLOW,energy=1):
    d=ImageDraw.Draw(im)
    for i in range(114):
        u=i/113
        env=(.22+.78*math.sin(math.pi*u)**.8)
        h=7+height*env*abs(math.sin(i*.52-t*3.4)*math.sin(i*.133+t*1.8))*energy
        bx=x+u*width
        d.rounded_rectangle((bx-2.6,y-h,bx+2.6,y+h),radius=3,fill=fill)


def motif_dots(im,t,color=YELLOW,alpha=1):
    d=ImageDraw.Draw(im)
    for i in range(17):
        a=i*2.399963;tangent=t*.08
        rad=240+18*i
        x=1470+math.cos(a+tangent)*rad
        y=520+math.sin(a+tangent)*rad*.67
        r=2 if i%4 else 5
        d.ellipse((x-r,y-r,x+r,y+r),fill=color)


def section_label(im,value,x,y,dark=False):
    d=ImageDraw.Draw(im)
    d.ellipse((x,y+8,x+8,y+16),fill=YELLOW if dark else INK)
    text(im,value,x+23,y,22,PAPER if dark else INK,'demi',tracking=2.8)


def scene_intro(t,F):
    im=background('ink').copy()
    d=ImageDraw.Draw(im)
    circles(im,1360,540,t,True)
    motif_dots(im,t)
    p=ease(t/1.4)
    # Moving light line precedes the object and resolves behind it.
    d.rounded_rectangle((1090,204,1624,814),radius=270,fill=(39,43,28))
    section_label(im,'A NEW WAY TO PICK THINGS UP',122,213,True)
    lines(im,['Your agents.','A little closer.'],116,306,110,PAPER,t,delay=.38)
    q=ease((t-1.5)/.8)
    d.line((122,598,122+190*q,598),fill=YELLOW,width=6)
    kinetic_line(im,'EVENER FOR PLAYDATE',124,654,28,YELLOW,t,1.8,'demi')
    kinetic_line(im,'A native client. A different kind of handheld.',124,715,27,(165,174,164),t,2.0,'regular')
    device(im,F.frame('hero',max(0,t)),1380,570+(1-p)*850+8*math.sin(t*1.2),1.16,-7+4*ease(t/4),t*.55)
    return im


def scene_setup(t,F):
    im=background('yellow').copy();d=ImageDraw.Draw(im)
    # Continuous travelling arcs carry the sound toward the handheld.
    for i in range(6):
        r=90+((t*115+i*143)%900)
        d.arc((680-r,565-r,680+r,565+r),-70,70,fill=(221,174,17),width=3)
    section_label(im,'01 / CONNECT',122,211)
    lines(im,['Start with','a chirp.'],116,310,126,INK,t,delay=.1)
    kinetic_line(im,'Pair using sound.',124,624,33,INK,t,.55,'medium')
    kinetic_line(im,"Listening for your Hub's invitation.",124,681,27,(106,87,21),t,.75,'regular')
    # The sound source is an abstract control glyph, not fabricated Hub UI.
    cx,cy=179,803
    d.ellipse((cx-52,cy-52,cx+52,cy+52),fill=INK)
    for i in range(4):
        hh=13+18*abs(math.sin(t*5+i*.9))
        d.rounded_rectangle((cx-25+i*17,cy-hh,cx-20+i*17,cy+hh),radius=3,fill=YELLOW)
    text(im,'SOUND TO DEVICE',257,790,23,INK,'demi',tracking=1.5)
    device(im,F.frame('setup',t),1380,565+5*math.sin(t),1.18,0,1.4+.18*math.sin(t))
    return im


def scene_read(t,F):
    im=background('paper').copy();d=ImageDraw.Draw(im)
    # A second camera composition opens the real conversation at generous scale.
    focus=smooth((t-3.0)/2.2)
    trackx=1801
    d.rounded_rectangle((trackx,243,trackx+7,812),radius=4,fill=(205,209,200))
    tracky=300+335*(.5+.5*math.sin(t*.49-1.1))
    d.rounded_rectangle((trackx-4,tracky,trackx+11,tracky+89),radius=8,fill=INK)
    if focus < 1:
        typography=Image.new('RGBA',(W,H))
        section_label(typography,'02 / KEEP READING',122,211)
        lines(typography,['Pick up the','conversation.'],116,310,107,INK,t,delay=.1)
        kinetic_line(typography,'Sessions. Projects. Your place in the story.',124,603,29,(93,103,96),t,.6,'regular')
        capsule(typography,'TURN THE CRANK',123,717,fill=INK,foreground=PAPER,size=23)
        kinetic_line(typography,'Scroll through what matters.',124,799,27,INK,t,1.1,'medium')
        if focus:
            typography.putalpha(typography.getchannel('A').point(lambda a:round(a*(1-smooth(focus*2)))))
        im.alpha_composite(typography,(-round(350*focus),0))
    device(im,F.frame('read',t),mix(1373,1075,focus),mix(584,620,focus),mix(1.21,1.48,focus),0,t*1.4+1)
    if focus:
        detail=smooth((focus-.5)*2)
        text(im,'THE CONVERSATION,',127,278,25,INK,'demi',alpha=detail,tracking=1.5)
        text(im,'IN FOCUS.',127,324,25,INK,'demi',alpha=detail,tracking=1.5)
        text(im,'Crank to scroll.',1510,751,27,INK,'medium',alpha=detail)
        text(im,'Keep your place.',1510,800,24,(94,108,93),'regular',alpha=detail)
    return im


def scene_voice(t,F):
    im=background('ink').copy()
    wave(im,t,72,870,1770,91,(107,111,40),.84)
    section_label(im,'03 / VOICE INPUT',122,211,True)
    lines(im,['Hold.','Speak.'],114,307,161,PAPER,t,delay=.1,gap=164)
    kinetic_line(im,'A thought is a good place to start.',124,691,29,(165,174,164),t,.65,'regular')
    capsule(im,'HOLD A TO DICTATE',124,768,fill=YELLOW,foreground=INK,size=23)
    # Traveling waveform crosses behind the device rather than obscuring pixels.
    circles(im,1400,558,t,True,730)
    device(im,F.frame('voice',t),1382,573+5*math.sin(t*.8),1.17,0,1.25,t+.1 if t<F.inputs['voice'].get('release',8) else 0)
    return im


def scene_review(t,F):
    im=background('yellow').copy();d=ImageDraw.Draw(im)
    section_label(im,'04 / MAKE IT YOURS',990,222)
    lines(im,['Read it.','Then send.'],983,320,115,INK,t,delay=.15,gap=120)
    kinetic_line(im,'Release to review.',992,622,31,INK,t,.75,'medium')
    kinetic_line(im,'Press A separately to send.',992,672,31,INK,t,1.,'medium')
    # Two editorial beats show the intentional boundary between review and send.
    d.line((1011,800,1537,800),fill=(193,145,11),width=3)
    for i,(xx,lab) in enumerate([(1011,'REVIEW'),(1476,'SEND')]):
        on=t>1.1+i*1.6
        d.ellipse((xx-17,783,xx+17,817),fill=INK if on else YELLOW,outline=INK,width=3)
        if on:d.ellipse((xx-5,795,xx+5,805),fill=YELLOW)
        text(im,lab,xx-1,844,21,INK,'demi',anchor='mt',tracking=None)
    p=clamp((t-2.5)/.7)
    d.line((1011,800,1011+p*465,800),fill=INK,width=4)
    device(im,F.frame('review',t),527,597,1.22,0,.7+.12*math.sin(t))
    return im


def scene_native(t,F):
    im=background('ink').copy();d=ImageDraw.Draw(im)
    section_label(im,'BUILT FOR THIS LITTLE MACHINE',122,202,True)
    # Native connection diagram is an illustration, with no service-availability claim.
    points=[(670,610),(859,610),(930,537),(1520,537)]
    rounded_line(d,points,(84,93,71),3)
    length=[0]
    for a,b in zip(points,points[1:]):length.append(length[-1]+math.dist(a,b))
    for k in range(6):
        dist=((t*125+k*155)%length[-1])
        for i in range(len(points)-1):
            if length[i]<=dist<=length[i+1]:
                p=(dist-length[i])/(length[i+1]-length[i]);x=mix(points[i][0],points[i+1][0],p);y=mix(points[i][1],points[i+1][1],p)
                d.ellipse((x-5,y-5,x+5,y+5),fill=YELLOW)
                break
    device(im,F.frame('hero',t),410,624,.76,0,t*.33,shadow=False)
    # Hub disc: abstract node/signal illustration, not fabricated app UI.
    d.ellipse((1510,406,1834,730),fill=(36,42,37),outline=(79,92,65),width=2)
    for i in range(3):
        yy=492+i*37
        d.rounded_rectangle((1618,yy,1724,yy+23),radius=5,outline=PAPER,width=2)
        d.ellipse((1631,yy+8,1637,yy+14),fill=YELLOW)
    text(im,'EVENER HUB',1672,766,22,(168,181,156),'demi',anchor='mt',tracking=None)
    # A triangle of typographic hierarchy, held long enough to read.
    kinetic_line(im,'Native C.',777,278,109,PAPER,t,.1)
    kinetic_line(im,'Your Hub.',777,414,109,PAPER,t,1.0)
    kinetic_line(im,'Your tailnet.',777,642,109,YELLOW,t,2.)
    kinetic_line(im,'Made to connect directly.',779,820,29,(167,178,164),t,2.6,'regular')
    return im


def scene_hero(t,F):
    im=background('yellow').copy()
    circles(im,1390,541,t,False)
    section_label(im,'TAKE THE CONVERSATION WITH YOU',122,212)
    lines(im,['Small screen.','Big ideas.'],116,311,117,INK,t,delay=.1,gap=125)
    kinetic_line(im,'Evener for Playdate',124,662,34,INK,t,.8,'demi')
    kinetic_line(im,'An experimental native client.',124,722,28,(99,85,30),t,1.,'regular')
    # A controlled floating hero rotation provides energy while screen content is real.
    device(im,F.frame('hero',t),1378,575+8*math.sin(t),1.2+.32*(1-ease(t/2.2)),-2+4*math.sin(t*.52),t*.85)
    return im


def scene_end(t,F):
    im=background('ink').copy()
    # Quiet orbit echoes the official mark without redrawing or altering it.
    circles(im,1519,520,t*.35,True,640)
    device(im,F.frame('hero',t),1480,570,.83,0,.8+math.sin(t*.6)*.15,shadow=False)
    section_label(im,'PRIME RADIANT',124,223,True)
    lines(im,['Evener','for Playdate.'],116,304,112,PAPER,t,delay=.1,gap=118)
    kinetic_line(im,'Experimental native client',124,599,27,(158,174,158),t,.55,'regular')
    p=ease((t-.45)/1.)
    mark=logo_at(450)
    logo=mark.copy()
    if p<.999:logo.putalpha(logo.getchannel('A').point(lambda a:round(a*p)))
    im.alpha_composite(logo,(124,706+round(30*(1-p))))
    kinetic_line(im,'primeradiant.com',126,864,33,YELLOW,t,.8,'demi')
    kinetic_line(im,'github.com/prime-radiant-inc/evener-client-playdate',126,943,22,(155,168,155),t,1.2,'medium')
    return im


SCENE_FNS={name:globals()['scene_'+name] for _,_,name in SCENES}


def frame(t,F,transition=True):
    index=next((i for i,(a,b,n) in enumerate(SCENES) if a<=t<b),len(SCENES)-1)
    start,end,name=SCENES[index];local=t-start
    im=SCENE_FNS[name](local,F)
    # Short directional wipes are timed to section downbeats. Pixel content remains intact.
    if transition and index>0 and local<.48:
        a,b,oldname=SCENES[index-1]
        if index not in F.transition_stills:
            F.transition_stills.clear()
            F.transition_stills[index]=SCENE_FNS[oldname](b-a-.001,F)
        prev=F.transition_stills[index]
        p=smooth(local/.48)
        mask=Image.new('L',(W,H));md=ImageDraw.Draw(mask)
        lead=-190+(W+380)*p
        md.polygon([(0,0),(lead+180,0),(lead-180,H),(0,H)],fill=255)
        im=Image.composite(im,prev,mask)
        edge=ImageDraw.Draw(im)
        edge.line((lead+180,0,lead-180,H),fill=YELLOW if index%2 else INK,width=9)
    else:
        F.transition_stills.clear()
    logo_bug(im,t,index,F.preview)
    if t<.35:
        black=Image.new('RGBA',im.size,INK+(255,));im=Image.blend(black,im,smooth(t/.35))
    return im.convert('RGB')


def render(args):
    F=Footage(args.manifest,args.preview)
    if args.frames:
        out=Path(args.out);out.mkdir(parents=True,exist_ok=True)
        for t in args.frames:
            f=frame(float(t),F,False);path=out/f'frame-{float(t):05.2f}.png';f.save(path)
            print(path,flush=True)
        F.close();return
    fps=args.fps
    total=round((args.end-args.start)*fps)
    width,height=([W,H] if not args.half else [960,540])
    cmd=['ffmpeg','-hide_banner','-loglevel','warning','-y','-f','rawvideo','-vcodec','rawvideo',
         '-pix_fmt','rgb24','-s',f'{width}x{height}','-r',str(fps),'-i','pipe:0']
    if args.audio:
        cmd += ['-ss',str(args.start),'-i',args.audio]
    cmd+=['-c:v','libx264','-preset',args.preset,'-crf',str(args.crf),'-pix_fmt','yuv420p',
          '-profile:v','high','-movflags','+faststart','-color_primaries','bt709',
          '-color_trc','bt709','-colorspace','bt709']
    if args.audio:cmd+=['-c:a','aac','-b:a','256k','-af','afade=t=out:st=57:d=1']
    cmd+=['-t',str(args.end-args.start),args.out]
    p=subprocess.Popen(cmd,stdin=subprocess.PIPE)
    begin=time.monotonic()
    try:
        for i in range(total):
            t=args.start+i/fps
            im=frame(t,F)
            if args.half: im=im.resize((960,540),Image.Resampling.LANCZOS)
            p.stdin.write(im.tobytes())
            if i%max(1,fps*2)==0:
                print(f'{i:4}/{total} | {t:5.1f}s | {i/max(.01,time.monotonic()-begin):.1f} fps',flush=True)
        p.stdin.close()
        if p.wait()!=0:raise RuntimeError('FFmpeg encode failed')
    finally:
        F.close()
    print(f'Completed: {args.out} ({time.monotonic()-begin:.1f}s)',flush=True)


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--manifest',default=str(ROOT/'footage.preview.json'))
    parser.add_argument('--out',default=str(ROOT/'promo-preview.mp4'))
    parser.add_argument('--frames',nargs='+',type=float)
    parser.add_argument('--audio')
    parser.add_argument('--start',type=float,default=0)
    parser.add_argument('--end',type=float,default=DURATION)
    parser.add_argument('--fps',type=int,default=FPS)
    parser.add_argument('--crf',type=int,default=18)
    parser.add_argument('--preset',default='medium')
    parser.add_argument('--preview',action='store_true')
    parser.add_argument('--half',action='store_true')
    render(parser.parse_args())
