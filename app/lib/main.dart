import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';
import 'package:path/path.dart' as p;
import 'package:path_provider/path_provider.dart';

import 'dawplay_bridge.dart';
import 'theme.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const DawplayApp());
}

class DawplayApp extends StatelessWidget {
  const DawplayApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'DAWPROJECT',
      debugShowCheckedModeBanner: false,
      theme: DawTheme.dark,
      home: const LibraryPage(),
    );
  }
}

class LibraryPage extends StatefulWidget {
  const LibraryPage({super.key});

  @override
  State<LibraryPage> createState() => _LibraryPageState();
}

class _LibraryPageState extends State<LibraryPage> {
  final List<String> _paths = [];
  int? _selected;
  String? _binary;
  bool _binaryReady = false;
  ProjectInfo? _info;
  String? _infoError;
  bool _infoLoading = false;
  String? _notice;
  int _infoGeneration = 0;
  int _deviceIndex = -1;
  List<String> _devices = const [];
  PlaybackSession? _playback;

  @override
  void initState() {
    super.initState();
    _restore();
  }

  Future<void> _restore() async {
    final binary = await findDawplayBinary();
    List<String> stored = [];
    try {
      final file = await _playlistFile();
      if (file.existsSync()) {
        final decoded = jsonDecode(await file.readAsString());
        if (decoded is Map<String, dynamic>) {
          final paths = decoded['paths'];
          if (paths is List) {
            stored = [for (final path in paths) if (path is String) path];
          }
          final device = decoded['device'];
          if (device is int) {
            _deviceIndex = device;
          }
        }
      }
    } catch (_) {
      stored = [];
    }
    if (!mounted) {
      return;
    }
    setState(() {
      _binary = binary;
      _binaryReady = true;
    });
    if (binary != null) {
      await _refreshDevices();
    }
    if (!mounted) {
      return;
    }
    setState(() {
      _paths
        ..clear()
        ..addAll(stored);
      if (_paths.isNotEmpty) {
        _selected = 0;
      }
    });
    if (_selected != null) {
      await _loadSelected();
    }
  }

  Future<File> _playlistFile() async {
    final directory = await getApplicationSupportDirectory();
    return File(p.join(directory.path, 'playlist.json'));
  }

  Future<void> _savePlaylist() async {
    try {
      final file = await _playlistFile();
      await file.parent.create(recursive: true);
      await file.writeAsString(jsonEncode({'paths': _paths, 'device': _deviceIndex}));
    } catch (_) {}
  }

  Future<void> _addProjects() async {
    final picked = await FilePicker.pickFiles(
      allowMultiple: true,
      type: FileType.custom,
      allowedExtensions: const ['dawproject'],
    );
    if (picked == null) {
      return;
    }
    final added = <String>[];
    for (final file in picked.files) {
      final path = file.path;
      if (path == null || _paths.contains(path) || added.contains(path)) {
        continue;
      }
      added.add(path);
    }
    if (added.isEmpty || !mounted) {
      return;
    }
    setState(() {
      final selectFirstAdded = _paths.isEmpty;
      _paths.addAll(added);
      if (selectFirstAdded) {
        _selected = 0;
      }
    });
    await _savePlaylist();
    if (_selected != null && _info == null && !_infoLoading) {
      await _loadSelected();
    }
  }

  void _removeProject(int index) {
    setState(() {
      _paths.removeAt(index);
      if (_paths.isEmpty) {
        _selected = null;
        _info = null;
        _infoError = null;
      } else if (_selected == index) {
        _selected = index.clamp(0, _paths.length - 1);
        _info = null;
      } else if (_selected != null && index < _selected!) {
        _selected = _selected! - 1;
      }
    });
    _savePlaylist();
    if (_selected != null && _info == null) {
      _loadSelected();
    }
  }

  Future<void> _select(int index) async {
    if (_selected == index && _info != null) {
      return;
    }
    await _stopPlayback();
    setState(() => _selected = index);
    await _loadSelected();
  }

  Future<void> _refreshDevices() async {
    final binary = _binary;
    if (binary == null) {
      return;
    }
    try {
      final devices = await listPlaybackDevices(binary);
      if (!mounted) {
        return;
      }
      setState(() {
        _devices = devices;
        if (_deviceIndex >= devices.length) {
          _deviceIndex = -1;
        }
      });
    } catch (error) {
      if (mounted) {
        setState(() => _notice = error.toString());
      }
    }
  }

  String get _deviceLabel {
    if (_deviceIndex < 0 || _deviceIndex >= _devices.length) {
      return 'System default';
    }
    return _devices[_deviceIndex];
  }

  Future<void> _chooseDevice() async {
    await _refreshDevices();
    if (!mounted) {
      return;
    }
    final chosen = await showDialog<int>(
      context: context,
      builder: (context) => _DeviceDialog(devices: _devices, selected: _deviceIndex),
    );
    if (chosen == null || !mounted) {
      return;
    }
    final wasPlaying = _playback != null;
    setState(() => _deviceIndex = chosen);
    await _savePlaylist();
    if (wasPlaying) {
      await _stopPlayback();
      await _play();
    }
  }

  Future<void> _stopPlayback() async {
    final playback = _playback;
    if (playback == null) {
      return;
    }
    setState(() => _playback = null);
    await playback.stop();
  }

  Future<void> _loadSelected() async {
    final index = _selected;
    if (index == null || index < 0 || index >= _paths.length) {
      return;
    }
    final path = _paths[index];
    final generation = ++_infoGeneration;
    setState(() {
      _infoLoading = true;
      _infoError = null;
      _notice = null;
    });
    try {
      final binary = _binary;
      if (binary == null) {
        throw DawplayException('dawplay was not found. Build it in the repo, or set DAWPLAY_BIN.');
      }
      final info = await readProjectInfo(binary, path);
      if (!mounted || generation != _infoGeneration) {
        return;
      }
      setState(() {
        _info = info;
        _infoLoading = false;
      });
    } catch (error) {
      if (!mounted || generation != _infoGeneration) {
        return;
      }
      setState(() {
        _info = null;
        _infoError = error.toString();
        _infoLoading = false;
      });
    }
  }

  Future<void> _play() async {
    if (_playback != null) {
      await _stopPlayback();
      return;
    }
    final index = _selected;
    final binary = _binary;
    if (index == null || binary == null) {
      return;
    }
    try {
      final session = await startPlayback(binary, _paths[index], _deviceIndex);
      if (!mounted) {
        await session.stop();
        return;
      }
      setState(() {
        _playback = session;
        _notice = null;
      });
      unawaited(session.process.exitCode.then((_) {
        if (!mounted || _playback != session) {
          return;
        }
        setState(() => _playback = null);
      }));
    } catch (error) {
      if (mounted) {
        setState(() => _notice = error.toString());
      }
    }
  }

  @override
  void dispose() {
    _playback?.process.kill();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Row(
        children: [
          _ProjectPane(
            paths: _paths,
            selected: _selected,
            deviceLabel: _deviceLabel,
            onAdd: _addProjects,
            onSelect: _select,
            onRemove: _removeProject,
            onDevice: _binary == null ? null : _chooseDevice,
          ),
          const VerticalDivider(width: 1, color: DawColors.border),
          Expanded(child: _detail()),
        ],
      ),
    );
  }

  Widget _detail() {
    final index = _selected;
    if (index == null) {
      return const _EmptyDetail();
    }
    final path = _paths[index];
    return Column(
      crossAxisAlignment: CrossAxisAlignment.stretch,
      children: [
        _DetailHeader(
          name: p.basename(path),
          path: path,
          canPlay: _binary != null && !_infoLoading,
          playing: _playback != null,
          onPlay: _play,
          onRefresh: _loadSelected,
        ),
        if (!_binaryReady)
          const LinearProgressIndicator(minHeight: 2, color: DawColors.accent)
        else if (_binary == null)
          const _Banner(
            message: 'dawplay was not found. Build it with cmake, or set DAWPLAY_BIN to the binary.',
          ),
        if (_notice != null) _Banner(message: _notice!),
        Expanded(child: _detailBody()),
      ],
    );
  }

  Widget _detailBody() {
    if (_infoLoading && _info == null) {
      return const Center(child: CircularProgressIndicator(strokeWidth: 2, color: DawColors.accent));
    }
    if (_infoError != null) {
      return Center(
        child: Padding(
          padding: const EdgeInsets.all(32),
          child: Text(
            _infoError!,
            textAlign: TextAlign.center,
            style: const TextStyle(color: DawColors.warning, fontSize: 15),
          ),
        ),
      );
    }
    final info = _info;
    if (info == null) {
      return const SizedBox.shrink();
    }
    return ListView(
      padding: const EdgeInsets.fromLTRB(28, 8, 28, 28),
      children: [
        Wrap(
          spacing: 10,
          runSpacing: 10,
          children: [
            _Stat(label: 'Application', value: info.application.isEmpty ? '(none)' : info.application),
            _Stat(label: 'Length', value: _formatLength(info.lengthSeconds)),
            _Stat(label: 'Events', value: '${info.events}'),
            _Stat(label: 'Tempo points', value: '${info.tempoPoints}'),
          ],
        ),
        const SizedBox(height: 28),
        const Text(
          'CHANNELS',
          style: TextStyle(color: DawColors.textLow, fontSize: 12, fontWeight: FontWeight.w700, letterSpacing: 0.6),
        ),
        const SizedBox(height: 10),
        if (info.channels.isEmpty)
          const Text('No channels', style: TextStyle(color: DawColors.textLow))
        else
          for (final channel in info.channels) _ChannelTile(channel: channel),
        if (info.warnings.isNotEmpty) ...[
          const SizedBox(height: 28),
          const Text(
            'WARNINGS',
            style: TextStyle(color: DawColors.textLow, fontSize: 12, fontWeight: FontWeight.w700, letterSpacing: 0.6),
          ),
          const SizedBox(height: 10),
          for (final warning in info.warnings)
            Padding(
              padding: const EdgeInsets.only(bottom: 8),
              child: Text(warning, style: const TextStyle(color: DawColors.warning, fontSize: 14)),
            ),
        ],
      ],
    );
  }
}

class _ProjectPane extends StatelessWidget {
  const _ProjectPane({
    required this.paths,
    required this.selected,
    required this.deviceLabel,
    required this.onAdd,
    required this.onSelect,
    required this.onRemove,
    required this.onDevice,
  });

  final List<String> paths;
  final int? selected;
  final String deviceLabel;
  final VoidCallback onAdd;
  final ValueChanged<int> onSelect;
  final ValueChanged<int> onRemove;
  final VoidCallback? onDevice;

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: 300,
      child: ColoredBox(
        color: DawColors.base,
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            const Padding(
              padding: EdgeInsets.fromLTRB(20, 22, 20, 4),
              child: Text(
                'Projects',
                style: TextStyle(color: DawColors.textHigh, fontSize: 18, fontWeight: FontWeight.w700),
              ),
            ),
            Padding(
              padding: const EdgeInsets.fromLTRB(8, 0, 8, 8),
              child: TextButton(
                onPressed: onDevice,
                style: TextButton.styleFrom(
                  foregroundColor: DawColors.textBody,
                  alignment: Alignment.centerLeft,
                  padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
                ),
                child: Row(
                  children: [
                    const Icon(Icons.speaker_rounded, size: 16, color: DawColors.accent),
                    const SizedBox(width: 8),
                    Expanded(
                      child: Text(
                        deviceLabel,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: const TextStyle(fontSize: 13),
                      ),
                    ),
                  ],
                ),
              ),
            ),
            Expanded(
              child: paths.isEmpty
                  ? const Center(
                      child: Text('No projects yet', style: TextStyle(color: DawColors.textLow, fontSize: 13)),
                    )
                  : ListView.builder(
                      itemCount: paths.length,
                      itemBuilder: (context, index) {
                        final path = paths[index];
                        final marked = index == selected;
                        return Material(
                          color: marked ? DawColors.accentMuted : Colors.transparent,
                          child: InkWell(
                            onTap: () => onSelect(index),
                            child: Padding(
                              padding: const EdgeInsets.fromLTRB(16, 10, 4, 10),
                              child: Row(
                                children: [
                                  Expanded(
                                    child: Column(
                                      crossAxisAlignment: CrossAxisAlignment.start,
                                      children: [
                                        Text(
                                          p.basename(path),
                                          maxLines: 1,
                                          overflow: TextOverflow.ellipsis,
                                          style: TextStyle(
                                            color: marked ? DawColors.accent : DawColors.textHigh,
                                            fontSize: 14,
                                            fontWeight: FontWeight.w600,
                                          ),
                                        ),
                                        const SizedBox(height: 2),
                                        Text(
                                          p.dirname(path),
                                          maxLines: 1,
                                          overflow: TextOverflow.ellipsis,
                                          style: const TextStyle(color: DawColors.textLow, fontSize: 11),
                                        ),
                                      ],
                                    ),
                                  ),
                                  IconButton(
                                    tooltip: 'Remove',
                                    onPressed: () => onRemove(index),
                                    icon: const Icon(Icons.close_rounded, size: 16, color: DawColors.textLow),
                                  ),
                                ],
                              ),
                            ),
                          ),
                        );
                      },
                    ),
            ),
            Padding(
              padding: const EdgeInsets.all(16),
              child: FilledButton.icon(
                onPressed: onAdd,
                style: FilledButton.styleFrom(
                  backgroundColor: DawColors.accent,
                  foregroundColor: Colors.black,
                  padding: const EdgeInsets.symmetric(vertical: 14),
                  shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
                ),
                icon: const Icon(Icons.add_rounded),
                label: const Text('Add DAWPROJECT', style: TextStyle(fontWeight: FontWeight.w700)),
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _DetailHeader extends StatelessWidget {
  const _DetailHeader({
    required this.name,
    required this.path,
    required this.canPlay,
    required this.playing,
    required this.onPlay,
    required this.onRefresh,
  });

  final String name;
  final String path;
  final bool canPlay;
  final bool playing;
  final VoidCallback onPlay;
  final VoidCallback onRefresh;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(28, 22, 20, 16),
      child: Row(
        children: [
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Text(name, style: const TextStyle(color: DawColors.textHigh, fontSize: 22, fontWeight: FontWeight.w700)),
                const SizedBox(height: 4),
                Text(path, maxLines: 1, overflow: TextOverflow.ellipsis, style: const TextStyle(color: DawColors.textLow, fontSize: 12)),
              ],
            ),
          ),
          IconButton(
            tooltip: 'Reload',
            onPressed: onRefresh,
            icon: const Icon(Icons.refresh_rounded, color: DawColors.textBody),
          ),
          const SizedBox(width: 4),
          FilledButton.icon(
            onPressed: canPlay ? onPlay : null,
            style: FilledButton.styleFrom(
              backgroundColor: DawColors.accent,
              foregroundColor: Colors.black,
              disabledBackgroundColor: DawColors.surface,
              disabledForegroundColor: DawColors.textLow,
              shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(8)),
            ),
            icon: Icon(playing ? Icons.stop_rounded : Icons.play_arrow_rounded),
            label: Text(playing ? 'Stop' : 'Play', style: const TextStyle(fontWeight: FontWeight.w700)),
          ),
        ],
      ),
    );
  }
}

class _DeviceDialog extends StatelessWidget {
  const _DeviceDialog({required this.devices, required this.selected});

  final List<String> devices;
  final int selected;

  @override
  Widget build(BuildContext context) {
    final choices = <int>[-1, for (var deviceIndex = 0; deviceIndex < devices.length; deviceIndex++) deviceIndex];
    return AlertDialog(
      backgroundColor: DawColors.base,
      title: const Text('Audio output', style: TextStyle(color: DawColors.textHigh, fontSize: 18)),
      content: SizedBox(
        width: 420,
        child: RadioGroup<int>(
          groupValue: selected,
          onChanged: (value) {
            if (value != null) {
              Navigator.pop(context, value);
            }
          },
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              for (final deviceIndex in choices)
                RadioListTile<int>(
                  value: deviceIndex,
                  activeColor: DawColors.accent,
                  title: Text(
                    deviceIndex < 0 ? 'System default' : devices[deviceIndex],
                    style: const TextStyle(color: DawColors.textHigh, fontSize: 15),
                  ),
                  contentPadding: EdgeInsets.zero,
                ),
            ],
          ),
        ),
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(context),
          child: const Text('Cancel', style: TextStyle(color: DawColors.textLow)),
        ),
      ],
    );
  }
}

class _EmptyDetail extends StatelessWidget {
  const _EmptyDetail();

  @override
  Widget build(BuildContext context) {
    return const Center(
      child: Text(
        'Add a .dawproject on the left.',
        style: TextStyle(color: DawColors.textLow, fontSize: 15),
      ),
    );
  }
}

class _Banner extends StatelessWidget {
  const _Banner({required this.message});

  final String message;

  @override
  Widget build(BuildContext context) {
    return Container(
      margin: const EdgeInsets.fromLTRB(28, 0, 28, 12),
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 10),
      decoration: BoxDecoration(
        color: DawColors.surface,
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: DawColors.border),
      ),
      child: Text(message, style: const TextStyle(color: DawColors.warning, fontSize: 13)),
    );
  }
}

class _Stat extends StatelessWidget {
  const _Stat({required this.label, required this.value});

  final String label;
  final String value;

  @override
  Widget build(BuildContext context) {
    return Container(
      width: 160,
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 12),
      decoration: BoxDecoration(
        color: DawColors.base,
        borderRadius: BorderRadius.circular(8),
        border: Border.all(color: DawColors.border),
      ),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Text(label, style: const TextStyle(color: DawColors.textLow, fontSize: 11, fontWeight: FontWeight.w700)),
          const SizedBox(height: 6),
          Text(value, style: const TextStyle(color: DawColors.textHigh, fontSize: 16, fontWeight: FontWeight.w600)),
        ],
      ),
    );
  }
}

class _ChannelTile extends StatelessWidget {
  const _ChannelTile({required this.channel});

  final ChannelRow channel;

  @override
  Widget build(BuildContext context) {
    return Container(
      margin: const EdgeInsets.only(bottom: 6),
      padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 12),
      decoration: BoxDecoration(
        color: DawColors.base,
        borderRadius: BorderRadius.circular(8),
      ),
      child: Row(
        children: [
          SizedBox(
            width: 28,
            child: Text('${channel.index}', style: const TextStyle(color: DawColors.textLow, fontSize: 13)),
          ),
          Expanded(
            child: Text(
              channel.name,
              style: const TextStyle(color: DawColors.textHigh, fontSize: 15, fontWeight: FontWeight.w600),
            ),
          ),
          Text(channel.role, style: const TextStyle(color: DawColors.textBody, fontSize: 13)),
          const SizedBox(width: 16),
          if (channel.solo)
            const Padding(
              padding: EdgeInsets.only(right: 16),
              child: Text('solo', style: TextStyle(color: DawColors.accent, fontSize: 13)),
            ),
          Text(channel.hardwareLabel, style: const TextStyle(color: DawColors.textLow, fontSize: 13)),
        ],
      ),
    );
  }
}

String _formatLength(double seconds) {
  final safe = seconds < 0 ? 0.0 : seconds;
  final whole = safe.floor();
  final minutes = whole ~/ 60;
  final remain = whole % 60;
  final tenths = ((safe - whole) * 10).floor().clamp(0, 9);
  return '$minutes:${remain.toString().padLeft(2, '0')}.$tenths';
}
