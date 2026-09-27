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
  [0] Drum Bus role=regular solo=no hw=-1/2
  [1] (unnamed) role=master solo=yes hw=0/2
Warning: Unrecognized warp algorithm
''';
    final info = ProjectInfo.parse(text);
    expect(info.application, 'Bitwig');
    expect(info.lengthSeconds, 125.5);
    expect(info.tempoPoints, 2);
    expect(info.events, 4);
    expect(info.channels, hasLength(2));
    expect(info.channels[0].name, 'Drum Bus');
    expect(info.channels[0].hardwareLabel, 'follow');
    expect(info.channels[1].solo, isTrue);
    expect(info.channels[1].hardwareLabel, 'out 1-2');
    expect(info.warnings, ['Unrecognized warp algorithm']);
  });
}
