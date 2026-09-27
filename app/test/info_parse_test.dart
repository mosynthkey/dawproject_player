import 'package:dawplay_app/dawplay_bridge.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  test('parses dawplay info text', () {
    const text = '''
Application: Bitwig
Length: 125.500 s
Tempo points: 2
Events: 4
Channels: 2
  [0] Drum Bus role=regular solo=no mute=no vol=0.500000 hw=-1/2
  [1] (unnamed) role=master solo=yes mute=yes vol=1.000000 hw=0/2
clip	0	1.000000	5.500000	Kick
clip	0	8.000000	12.250000	Snare Roll
Warning: Unrecognized warp algorithm
''';
    final info = ProjectInfo.parse(text);
    expect(info.application, 'Bitwig');
    expect(info.lengthSeconds, 125.5);
    expect(info.tempoPoints, 2);
    expect(info.events, 4);
    expect(info.channels, hasLength(2));
    expect(info.channels[0].name, 'Drum Bus');
    expect(info.channels[0].muted, isFalse);
    expect(info.channels[0].volume, 0.5);
    expect(info.channels[0].hardwareLabel, 'follow');
    expect(info.channels[1].solo, isTrue);
    expect(info.channels[1].muted, isTrue);
    expect(info.channels[1].hardwareLabel, 'out 1-2');
    expect(info.clips, hasLength(2));
    expect(info.clips[0].name, 'Kick');
    expect(info.clips[0].start, 1);
    expect(info.clips[0].end, 5.5);
    expect(info.clips[1].name, 'Snare Roll');
    expect(info.warnings, ['Unrecognized warp algorithm']);
  });
}
