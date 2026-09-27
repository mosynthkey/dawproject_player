import 'dart:async';
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
    required this.hardwareStart,
    required this.hardwareWidth,
  });

  final int index;
  final String name;
  final String role;
  final bool solo;
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
      r'^\s*\[(\d+)\]\s+(.*)\s+role=(\S+)\s+solo=(yes|no)\s+hw=(-?\d+)/(\d+)\s*$',
    ).firstMatch(line);
    if (match == null) {
      return null;
    }
    return ChannelRow(
      index: int.parse(match.group(1)!),
      name: match.group(2)!,
      role: match.group(3)!,
      solo: match.group(4) == 'yes',
      hardwareStart: int.parse(match.group(5)!),
      hardwareWidth: int.parse(match.group(6)!),
    );
  }
}

class ProjectInfo {
  const ProjectInfo({
    required this.application,
    required this.lengthSeconds,
    required this.tempoPoints,
    required this.events,
    required this.channels,
    required this.warnings,
  });

  final String application;
  final double lengthSeconds;
  final int tempoPoints;
  final int events;
  final List<ChannelRow> channels;
  final List<String> warnings;

  static ProjectInfo parse(String text) {
    var application = '';
    var lengthSeconds = 0.0;
    var tempoPoints = 0;
    var events = 0;
    final channels = <ChannelRow>[];
    final warnings = <String>[];
    for (final line in text.split('\n')) {
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
  PlaybackSession(this.process);

  final Process process;

  Future<void> stop() async {
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

Future<PlaybackSession> startPlayback(String binary, String projectPath, int deviceIndex) async {
  final arguments = <String>['play', projectPath];
  if (deviceIndex >= 0) {
    arguments.addAll(['--device', '$deviceIndex']);
  }
  final process = await Process.start(binary, arguments);
  final errors = StringBuffer();
  process.stderr.transform(systemEncoding.decoder).listen(errors.write);
  final ready = Completer<void>();
  final output = StringBuffer();
  process.stdout.transform(systemEncoding.decoder).listen((chunk) {
    output.write(chunk);
    if (!ready.isCompleted && output.toString().contains('playing')) {
      ready.complete();
    }
  });
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
    if (error is TimeoutException) {
      throw DawplayException('Playback did not start');
    }
    rethrow;
  }
  return PlaybackSession(process);
}
