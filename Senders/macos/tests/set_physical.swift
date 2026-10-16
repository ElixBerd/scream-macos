// Sets the Scream Audio stream's physical format, like the Format menu in
// Audio MIDI Setup does. Usage: swift set_physical.swift BITS
import CoreAudio
import Foundation

let bits = UInt32(CommandLine.arguments[1])!
var uid = "ScreamAudio_UID" as CFString
var dev = AudioObjectID(0)
var size = UInt32(MemoryLayout<AudioObjectID>.size)
var a = AudioObjectPropertyAddress(mSelector: kAudioHardwarePropertyTranslateUIDToDevice,
                                   mScope: kAudioObjectPropertyScopeGlobal, mElement: kAudioObjectPropertyElementMain)
var st = withUnsafeMutablePointer(to: &uid) {
  AudioObjectGetPropertyData(AudioObjectID(kAudioObjectSystemObject), &a,
                             UInt32(MemoryLayout<CFString>.size), $0, &size, &dev)
}
guard st == noErr, dev != 0 else { print("Scream Audio not found"); exit(1) }

var stream = AudioObjectID(0)
size = UInt32(MemoryLayout<AudioObjectID>.size)
a = AudioObjectPropertyAddress(mSelector: kAudioDevicePropertyStreams, mScope: kAudioObjectPropertyScopeOutput,
                               mElement: kAudioObjectPropertyElementMain)
st = AudioObjectGetPropertyData(dev, &a, 0, nil, &size, &stream)
guard st == noErr else { print("no stream: \(st)"); exit(1) }

var fmt = AudioStreamBasicDescription()
size = UInt32(MemoryLayout<AudioStreamBasicDescription>.size)
a.mSelector = kAudioStreamPropertyPhysicalFormat
a.mScope = kAudioObjectPropertyScopeGlobal
AudioObjectGetPropertyData(stream, &a, 0, nil, &size, &fmt)
print("before: \(fmt.mBitsPerChannel) bit, flags \(fmt.mFormatFlags)")
fmt.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked
fmt.mBitsPerChannel = bits
fmt.mBytesPerFrame = 2 * bits / 8
fmt.mBytesPerPacket = fmt.mBytesPerFrame
st = AudioObjectSetPropertyData(stream, &a, 0, nil, size, &fmt)
guard st == noErr else { print("set failed: \(st)"); exit(1) }
Thread.sleep(forTimeInterval: 1)
AudioObjectGetPropertyData(stream, &a, 0, nil, &size, &fmt)
print("after: \(fmt.mBitsPerChannel) bit")
exit(fmt.mBitsPerChannel == bits ? 0 : 1)
