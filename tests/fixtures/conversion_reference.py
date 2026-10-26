"""CC0: independent binary64 reference, no miniaudio or codec calls.
Eight-pole Butterworth: four RBJ low-pass sections, Q=1/(2*cos(theta)).
Linear interpolation has one input-frame delay. Downsampling filters first;
upsampling filters last. Trim floor(9*out/in) reported latency, retain ceil(N*out/in).
Run from any directory; prints the literal expectations in test_source.c.
"""
import math


def lowpass(samples):
    for k in range(4):
        q = 1 / (2 * math.cos((1 + 2*k) * math.pi / 16))
        alpha = 1 / (2*q)  # cutoff pi/2 for the 2:1 and 1:2 references
        b = [0.5/(1+alpha), 1/(1+alpha), 0.5/(1+alpha)]
        a2 = (1-alpha)/(1+alpha)
        x1 = x2 = y1 = y2 = 0.0
        output = []
        for x in samples:
            y = b[0]*x + b[1]*x1 + b[2]*x2 - a2*y2
            output.append(y)
            x2, x1, y2, y1 = x1, x, y1, y
        samples = output
    return samples


signal = [(i*1000-8000)/32768 for i in range(16)] + [0.0]*32
up = [0.0, signal[0]/2]
for left, right in zip(signal, signal[1:]):
    up.extend((left, (left+right)/2))
filtered = lowpass(signal)
for name, values in [("up", lowpass(up)[18:50]), ("down", [0.0]+filtered[1::2])]:
    if name == "down":
        values = values[4:12]
    print(name, ", ".join(f"{x:.10g}f" for x in values))
