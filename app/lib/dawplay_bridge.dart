import 'dart:async';
import 'dart:convert';
import 'dart:io';

class DawplayException implements Exception {
  DawplayException(this.message);

  final String message;

  @override
  String toString() => message;
}

class ChannelRow {
  const ChannelRow({
    required this.index,
    required this.name,
    required this.role,
    required this.solo,
    required this.muted,
    required this.volume,
    required this.hardwareStart,
    required this.hardwareWidth,
  });

  final int index;
  final String name;
  final String role;
  final bool solo;
  final bool muted;
  final double volume;
  final int hardwareStart;
  final int hardwareWidth;

  String get hardwareLabel {
    if (hardwareStart < 0) {
      return 'follow';
    }
    if (hardwareWidth <= 1) {
      return 'out ${hardwareStart + 1}';
    }
    return 'out ${hardwareStart + 1}-${hardwareStart + hardwareWidth}';
  }

  static ChannelRow? tryParse(String line) {
    final match = RegExp(
      r'^\s*\[(\d+)\]\s+(.*)\s+role=(\S+)\s+solo=(yes|no)\s+mute=(yes|no)\s+vol=(\S+)\s+hw=(-?\d+)/(\d+)\s*$',
    ).firstMatch(line);
    if (match == null) {
      return null;
    }
    return ChannelRow(
      index: int.parse(match.group(1)!),
      name: match.group(2)!,
      role: match.group(3)!,
      solo: match.group(4) == 'yes',
      muted: match.group(5) == 'yes',
      volume: double.tryParse(match.group(6)!) ?? 1,
      hardwareStart: int.parse(match.group(7)!),
      hardwareWidth: int.parse(match.group(8)!),
    );
  }
}

class ClipSpan {
  const ClipSpan({required this.channelIndex, required this.start, required this.end, required this.name});

  final int channelIndex;
  final double start;
  final double end;
  final String name;

  static ClipSpan? tryParse(String line) {
    if (!line.startsWith('clip\t')) {
      return null;
    }
    final parts = line.split('\t');
    if (parts.length < 5) {
      return null;
    }
    final channelIndex = int.tryParse(parts[1]);
    final start = double.tryParse(parts[2]);
    final end = double.tryParse(parts[3]);
    if (channelIndex == null || start == null || end == null || end <= start) {
      return null;
    }
    return ClipSpan(channelIndex: channelIndex, start: start, end: end, name: parts.sublist(4).join('\t'));
  }
}

class ProjectInfo {
  const ProjectInfo({
    required this.application,
    required this.lengthSeconds,
    required this.tempoPoints,
    required this.events,
    required this.channels,
    required this.clips,
    required this.warnings,
  });

  final String application;
  final double lengthSeconds;
  final int tempoPoints;
  final int events;
  final List<ChannelRow> channels;
  final List<ClipSpan> clips;
  final List<String> warnings;

  static ProjectInfo parse(String text) {
    var application = '';
    var lengthSeconds = 0.0;
    var tempoPoints = 0;
    var events = 0;
    final channels = <ChannelRow>[];
    final clips = <ClipSpan>[];
    final warnings = <String>[];
    for (final raw in text.split('\n')) {
      final line = raw.trimRight();
      if (line.startsWith('Application: ')) {
        application = line.substring('Application: '.length).trim();
      } else if (line.startsWith('Length: ')) {
        final number = line.substring('Length: '.length).replaceAll('s', '').trim();
        lengthSeconds = double.tryParse(number) ?? 0;
      } else if (line.startsWith('Tempo points: ')) {
        tempoPoints = int.tryParse(line.substring('Tempo points: '.length).trim()) ?? 0;
      } else if (line.startsWith('Events: ')) {
        events = int.tryParse(line.substring('Events: '.length).trim()) ?? 0;
      } else if (line.startsWith('Warning: ')) {
        warnings.add(line.substring('Warning: '.length).trim());
      } else {
        final clip = ClipSpan.tryParse(line);
        if (clip != null) {
          clips.add(clip);
          continue;
        }
        final channel = ChannelRow.tryParse(line);
        if (channel != null) {
          channels.add(channel);
        }
      }
    }
    return ProjectInfo(
      application: application,
      lengthSeconds: lengthSeconds,
      tempoPoints: tempoPoints,
      events: events,
      channels: channels,
      clips: clips,
      warnings: warnings,
    );
  }
}

Future<String?> findDawplayBinary() async {
  final fromEnvironment = Platform.environment['DAWPLAY_BIN'];
  if (fromEnvironment != null && fromEnvironment.isNotEmpty && File(fromEnvironment).existsSync()) {
    return fromEnvironment;
  }

  final starts = <Directory>[
    Directory.current,
    File(Platform.resolvedExecutable).parent,
  ];
  for (final start in starts) {
    var directory = start;
    for (var depth = 0; depth < 12; depth++) {
      final candidate = File('${directory.path}/build/dawplay');
      if (candidate.existsSync()) {
        return candidate.path;
      }
      final parent = directory.parent;
      if (parent.path == directory.path) {
        break;
      }
      directory = parent;
    }
  }
  return null;
}

Future<ProjectInfo> readProjectInfo(String binary, String projectPath) async {
  final result = await Process.run(binary, ['info', projectPath]);
  if (result.exitCode != 0) {
    final error = '${result.stderr}'.trim();
    throw DawplayException(error.isEmpty ? 'Could not read the project' : error);
  }
  return ProjectInfo.parse('${result.stdout}');
}

Future<List<String>> listPlaybackDevices(String binary) async {
  final result = await Process.run(binary, ['devices']);
  final names = <String>[];
  for (final line in '${result.stdout}'.split('\n')) {
    if (line.trim().isEmpty) {
      continue;
    }
    final tab = line.indexOf('\t');
    names.add(tab < 0 ? line.trim() : line.substring(tab + 1));
  }
  if (result.exitCode != 0 && names.isEmpty) {
    final error = '${result.stderr}'.trim();
    if (error.isNotEmpty && error != 'No playback devices') {
      throw DawplayException(error);
    }
  }
  return names;
}

class PlaybackSession {
  PlaybackSession(this.process, this._positions);

  final Process process;
  final StreamController<double> _positions;

  Stream<double> get positions => _positions.stream;

  Future<void> command(String line) async {
    try {
      process.stdin.writeln(line);
      await process.stdin.flush();
    } catch (_) {}
  }

  Future<void> stop() async {
    if (!_positions.isClosed) {
      await _positions.close();
    }
    try {
      await process.stdin.close();
    } catch (_) {}
    try {
      await process.exitCode.timeout(const Duration(seconds: 2));
    } catch (_) {
      process.kill();
    }
  }
}

Future<PlaybackSession> startPlayback(String binary, String projectPath, int deviceIndex, {double seekSeconds = 0}) async {
  final arguments = <String>['play', projectPath];
  if (deviceIndex >= 0) {
    arguments.addAll(['--device', '$deviceIndex']);
  }
  if (seekSeconds > 0.001) {
    arguments.addAll(['--seek', seekSeconds.toStringAsFixed(3)]);
  }
  final process = await Process.start(binary, arguments);
  final positions = StreamController<double>();
  final errors = StringBuffer();
  process.stderr.transform(systemEncoding.decoder).listen(errors.write);
  final ready = Completer<void>();
  var pending = '';
  process.stdout.transform(utf8.decoder).listen(
    (chunk) {
      pending += chunk;
      while (true) {
        final newline = pending.indexOf('\n');
        if (newline < 0) {
          break;
        }
        final line = pending.substring(0, newline).trim();
        pending = pending.substring(newline + 1);
        if (line == 'playing' && !ready.isCompleted) {
          ready.complete();
        } else if (line.startsWith('pos ')) {
          final seconds = double.tryParse(line.substring(4));
          if (seconds != null && !positions.isClosed) {
            positions.add(seconds);
          }
        }
      }
    },
    onDone: () {
      if (!positions.isClosed) {
        positions.close();
      }
    },
  );
  unawaited(process.exitCode.then((code) {
    if (ready.isCompleted) {
      return;
    }
    final message = errors.toString().trim();
    ready.completeError(DawplayException(message.isEmpty ? 'Playback failed ($code)' : message));
  }));
  try {
    await ready.future.timeout(const Duration(seconds: 30));
  } catch (error) {
    process.kill();
    if (!positions.isClosed) {
      await positions.close();
    }
    if (error is TimeoutException) {
      throw DawplayException('Playback did not start');
    }
    rethrow;
  }
  return PlaybackSession(process, positions);
}
