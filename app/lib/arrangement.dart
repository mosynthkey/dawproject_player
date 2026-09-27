import 'dart:math' as math;

import 'package:flutter/material.dart';

import 'dawplay_bridge.dart';
import 'theme.dart';

class TrackMix {
  TrackMix({required this.volume, required this.muted, required this.solo});

  double volume;
  bool muted;
  bool solo;
  bool volumeHeld = false;
  bool muteHeld = false;
  bool soloHeld = false;
}

const arrangementHeaderWidth = 268.0;
const arrangementRulerHeight = 32.0;
const arrangementRowHeight = 76.0;

const _clipColors = <Color>[
  Color(0xFF3E6F8F),
  Color(0xFF4C6E52),
  Color(0xFF6E5C40),
  Color(0xFF5C5180),
  Color(0xFF724858),
  Color(0xFF3E6864),
];

String formatClock(double seconds) {
  final safe = seconds.isFinite && seconds > 0 ? seconds : 0.0;
  final whole = safe.floor();
  final minutes = whole ~/ 60;
  final remain = whole % 60;
  final tenths = ((safe - whole) * 10).floor().clamp(0, 9);
  return '$minutes:${remain.toString().padLeft(2, '0')}.$tenths';
}

double decibelsOf(double linear) {
  if (linear <= 1.0e-4) {
    return -60;
  }
  final decibels = 20 * math.log(linear) / math.ln10;
  return decibels.clamp(-60.0, 6.0);
}

double linearOfDecibels(double decibels) {
  if (decibels <= -60) {
    return 0;
  }
  return math.pow(10, decibels / 20).toDouble();
}

String formatDecibels(double linear) {
  if (linear <= 1.0e-4) {
    return '-inf';
  }
  return '${decibelsOf(linear).toStringAsFixed(1)} dB';
}

double rulerStep(double pixelsPerSecond) {
  const candidates = <double>[0.5, 1, 2, 5, 10, 15, 30, 60, 120, 300];
  for (final step in candidates) {
    if (step * pixelsPerSecond >= 80) {
      return step;
    }
  }
  return 300;
}

class ArrangementView extends StatefulWidget {
  const ArrangementView({
    super.key,
    required this.channels,
    required this.clips,
    required this.mix,
    required this.lengthSeconds,
    required this.position,
    required this.followPlayhead,
    required this.onSeek,
    required this.onScrubStart,
    required this.onScrub,
    required this.onScrubEnd,
    required this.onVolume,
    required this.onMute,
    required this.onSolo,
  });

  final List<ChannelRow> channels;
  final List<ClipSpan> clips;
  final List<TrackMix> mix;
  final double lengthSeconds;
  final ValueNotifier<double> position;
  final bool followPlayhead;
  final ValueChanged<double> onSeek;
  final VoidCallback onScrubStart;
  final ValueChanged<double> onScrub;
  final VoidCallback onScrubEnd;
  final void Function(int channelIndex, double linear) onVolume;
  final ValueChanged<int> onMute;
  final ValueChanged<int> onSolo;

  @override
  State<ArrangementView> createState() => _ArrangementViewState();
}

class _ArrangementViewState extends State<ArrangementView> {
  final ScrollController _headerVertical = ScrollController();
  final ScrollController _laneVertical = ScrollController();
  final ScrollController _laneHorizontal = ScrollController();
  final ScrollController _rulerHorizontal = ScrollController();
  bool _scrollLock = false;
  bool _dragging = false;
  double _pixelsPerSecond = 48;

  @override
  void initState() {
    super.initState();
    widget.position.addListener(_onPosition);
    _bind(_headerVertical, _laneVertical);
    _bind(_laneVertical, _headerVertical);
    _bind(_laneHorizontal, _rulerHorizontal);
  }

  @override
  void didUpdateWidget(ArrangementView oldWidget) {
    super.didUpdateWidget(oldWidget);
    if (oldWidget.position != widget.position) {
      oldWidget.position.removeListener(_onPosition);
      widget.position.addListener(_onPosition);
    }
  }

  void _onPosition() {
    if (_dragging || !widget.followPlayhead) {
      return;
    }
    _revealPlayhead();
  }

  void _bind(ScrollController source, ScrollController target) {
    source.addListener(() {
      if (_scrollLock || !source.hasClients || !target.hasClients) {
        return;
      }
      if ((source.offset - target.offset).abs() < 0.5) {
        return;
      }
      _scrollLock = true;
      final maxOffset = target.position.maxScrollExtent;
      target.jumpTo(source.offset.clamp(0.0, maxOffset));
      _scrollLock = false;
    });
  }

  void _revealPlayhead() {
    if (!mounted || !_laneHorizontal.hasClients) {
      return;
    }
    final playhead = widget.position.value * _pixelsPerSecond;
    final offset = _laneHorizontal.offset;
    final viewport = _laneHorizontal.position.viewportDimension;
    if (playhead < offset + viewport - 32) {
      return;
    }
    final target = playhead - viewport * 0.25;
    _laneHorizontal.jumpTo(target.clamp(0.0, _laneHorizontal.position.maxScrollExtent));
  }

  @override
  void dispose() {
    widget.position.removeListener(_onPosition);
    _headerVertical.dispose();
    _laneVertical.dispose();
    _laneHorizontal.dispose();
    _rulerHorizontal.dispose();
    super.dispose();
  }

  double _secondsAt(double dx) {
    if (_pixelsPerSecond <= 0) {
      return 0;
    }
    final length = widget.lengthSeconds;
    final seconds = dx / _pixelsPerSecond;
    if (length <= 0) {
      return 0;
    }
    return seconds.clamp(0.0, length);
  }

  @override
  Widget build(BuildContext context) {
    if (widget.channels.isEmpty) {
      return const Center(
        child: Text('No channels', style: TextStyle(color: DawColors.textLow, fontSize: 15)),
      );
    }
    return LayoutBuilder(
      builder: (context, constraints) {
        final laneViewport = math.max(120.0, constraints.maxWidth - arrangementHeaderWidth);
        final length = widget.lengthSeconds;
        final fit = length > 0.05 ? laneViewport / length : 48.0;
        final pixelsPerSecond = math.max(fit, 28.0);
        _pixelsPerSecond = pixelsPerSecond;
        final contentWidth = math.max(laneViewport, (length > 0 ? length : 1) * pixelsPerSecond);
        final lanesHeight = widget.channels.length * arrangementRowHeight;
        final laneViewportHeight = math.max(1.0, constraints.maxHeight - arrangementRulerHeight);
        return Row(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            SizedBox(
              width: arrangementHeaderWidth,
              child: Column(
                children: [
                  ListenableBuilder(
                    listenable: widget.position,
                    builder: (context, _) => _ClockCorner(position: widget.position.value, length: length),
                  ),
                  Expanded(
                    child: ListView.builder(
                      controller: _headerVertical,
                      primary: false,
                      padding: EdgeInsets.zero,
                      physics: const ClampingScrollPhysics(),
                      itemExtent: arrangementRowHeight,
                      itemCount: widget.channels.length,
                      itemBuilder: (context, channelIndex) {
                        final channel = widget.channels[channelIndex];
                        final mix = channelIndex < widget.mix.length ? widget.mix[channelIndex] : null;
                        return _TrackHeader(
                          channel: channel,
                          mix: mix,
                          shaded: channelIndex.isOdd,
                          onVolume: (linear) => widget.onVolume(channel.index, linear),
                          onMute: () => widget.onMute(channel.index),
                          onSolo: () => widget.onSolo(channel.index),
                        );
                      },
                    ),
                  ),
                ],
              ),
            ),
            Expanded(
              child: Column(
                children: [
                  SizedBox(
                    height: arrangementRulerHeight,
                    child: SingleChildScrollView(
                      controller: _rulerHorizontal,
                      scrollDirection: Axis.horizontal,
                      physics: const NeverScrollableScrollPhysics(),
                      child: GestureDetector(
                        behavior: HitTestBehavior.opaque,
                        onTapDown: (details) => widget.onSeek(_secondsAt(details.localPosition.dx)),
                        onHorizontalDragStart: (details) {
                          _dragging = true;
                          widget.onScrubStart();
                          widget.onScrub(_secondsAt(details.localPosition.dx));
                        },
                        onHorizontalDragUpdate: (details) => widget.onScrub(_secondsAt(details.localPosition.dx)),
                        onHorizontalDragEnd: (_) {
                          _dragging = false;
                          widget.onScrubEnd();
                        },
                        onHorizontalDragCancel: () {
                          _dragging = false;
                          widget.onScrubEnd();
                        },
                        child: ListenableBuilder(
                          listenable: widget.position,
                          builder: (context, _) => CustomPaint(
                            size: Size(contentWidth, arrangementRulerHeight),
                            painter: _RulerPainter(
                              lengthSeconds: length,
                              positionSeconds: widget.position.value,
                              pixelsPerSecond: pixelsPerSecond,
                            ),
                          ),
                        ),
                      ),
                    ),
                  ),
                  Expanded(
                    child: Scrollbar(
                      controller: _laneVertical,
                      notificationPredicate: (notification) => notification.metrics.axis == Axis.vertical,
                      child: Scrollbar(
                        controller: _laneHorizontal,
                        notificationPredicate: (notification) => notification.metrics.axis == Axis.horizontal,
                        child: SingleChildScrollView(
                          controller: _laneHorizontal,
                          scrollDirection: Axis.horizontal,
                          physics: const ClampingScrollPhysics(),
                          child: SizedBox(
                            width: contentWidth,
                            height: laneViewportHeight,
                            child: SingleChildScrollView(
                              controller: _laneVertical,
                              physics: const ClampingScrollPhysics(),
                              child: GestureDetector(
                                behavior: HitTestBehavior.opaque,
                                onTapUp: (details) => widget.onSeek(_secondsAt(details.localPosition.dx)),
                                child: ListenableBuilder(
                                  listenable: widget.position,
                                  builder: (context, _) => CustomPaint(
                                    size: Size(contentWidth, lanesHeight),
                                    painter: _LanePainter(
                                      channels: widget.channels.length,
                                      clips: widget.clips,
                                      lengthSeconds: length,
                                      positionSeconds: widget.position.value,
                                      pixelsPerSecond: pixelsPerSecond,
                                    ),
                                  ),
                                ),
                              ),
                            ),
                          ),
                        ),
                      ),
                    ),
                  ),
                ],
              ),
            ),
          ],
        );
      },
    );
  }
}

class _ClockCorner extends StatelessWidget {
  const _ClockCorner({required this.position, required this.length});

  final double position;
  final double length;

  @override
  Widget build(BuildContext context) {
    return Container(
      height: arrangementRulerHeight,
      alignment: Alignment.centerLeft,
      padding: const EdgeInsets.symmetric(horizontal: 14),
      decoration: const BoxDecoration(
        color: DawColors.base,
        border: Border(right: BorderSide(color: DawColors.border), bottom: BorderSide(color: DawColors.border)),
      ),
      child: Text.rich(
        TextSpan(
          text: formatClock(position),
          style: const TextStyle(color: DawColors.accent, fontSize: 13, fontWeight: FontWeight.w700, fontFeatures: [FontFeature.tabularFigures()]),
          children: [
            TextSpan(
              text: '  /  ${formatClock(length)}',
              style: const TextStyle(color: DawColors.textLow, fontWeight: FontWeight.w500),
            ),
          ],
        ),
      ),
    );
  }
}

class _TrackHeader extends StatelessWidget {
  const _TrackHeader({
    required this.channel,
    required this.mix,
    required this.shaded,
    required this.onVolume,
    required this.onMute,
    required this.onSolo,
  });

  final ChannelRow channel;
  final TrackMix? mix;
  final bool shaded;
  final ValueChanged<double> onVolume;
  final VoidCallback onMute;
  final VoidCallback onSolo;

  @override
  Widget build(BuildContext context) {
    final volume = mix?.volume ?? channel.volume;
    final muted = mix?.muted ?? channel.muted;
    final solo = mix?.solo ?? channel.solo;
    final role = channel.role == 'regular' ? '' : channel.role;
    return DecoratedBox(
      decoration: BoxDecoration(
        color: shaded ? DawColors.laneAlt : DawColors.appBg,
        border: const Border(right: BorderSide(color: DawColors.border), bottom: BorderSide(color: DawColors.border)),
      ),
      child: Padding(
        padding: const EdgeInsets.fromLTRB(12, 8, 8, 4),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Row(
              children: [
                Expanded(
                  child: Text(
                    channel.name,
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: const TextStyle(color: DawColors.textHigh, fontSize: 13, fontWeight: FontWeight.w600),
                  ),
                ),
                if (role.isNotEmpty)
                  Text(role, style: const TextStyle(color: DawColors.textLow, fontSize: 11)),
              ],
            ),
            const SizedBox(height: 4),
            Row(
              children: [
                _MixButton(label: 'M', tooltip: 'Mute', active: muted, activeColor: DawColors.mute, onTap: onMute),
                const SizedBox(width: 4),
                _MixButton(label: 'S', tooltip: 'Solo', active: solo, activeColor: DawColors.solo, onTap: onSolo),
                Expanded(
                  child: SliderTheme(
                    data: SliderTheme.of(context).copyWith(
                      trackHeight: 2,
                      activeTrackColor: DawColors.accent,
                      inactiveTrackColor: DawColors.border,
                      thumbColor: DawColors.textHigh,
                      overlayColor: DawColors.accentMuted,
                      thumbShape: const RoundSliderThumbShape(enabledThumbRadius: 5),
                      overlayShape: const RoundSliderOverlayShape(overlayRadius: 10),
                    ),
                    child: Slider(
                      min: -60,
                      max: 6,
                      value: decibelsOf(volume),
                      onChanged: (decibels) => onVolume(linearOfDecibels(decibels)),
                    ),
                  ),
                ),
                SizedBox(
                  width: 52,
                  child: Text(
                    formatDecibels(volume),
                    textAlign: TextAlign.right,
                    style: const TextStyle(color: DawColors.textLow, fontSize: 10, fontFeatures: [FontFeature.tabularFigures()]),
                  ),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

class _MixButton extends StatelessWidget {
  const _MixButton({
    required this.label,
    required this.tooltip,
    required this.active,
    required this.activeColor,
    required this.onTap,
  });

  final String label;
  final String tooltip;
  final bool active;
  final Color activeColor;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return Tooltip(
      message: tooltip,
      child: GestureDetector(
        onTap: onTap,
        child: Container(
          width: 22,
          height: 22,
          alignment: Alignment.center,
          decoration: BoxDecoration(
            color: active ? activeColor : Colors.transparent,
            borderRadius: BorderRadius.circular(4),
            border: Border.all(color: active ? activeColor : const Color(0x33FFFFFF)),
          ),
          child: Text(
            label,
            style: TextStyle(
              color: active ? Colors.black : DawColors.textLow,
              fontSize: 11,
              fontWeight: FontWeight.w800,
            ),
          ),
        ),
      ),
    );
  }
}

class _RulerPainter extends CustomPainter {
  _RulerPainter({required this.lengthSeconds, required this.positionSeconds, required this.pixelsPerSecond});

  final double lengthSeconds;
  final double positionSeconds;
  final double pixelsPerSecond;

  @override
  void paint(Canvas canvas, Size size) {
    canvas.drawRect(Offset.zero & size, Paint()..color = DawColors.base);
    final step = rulerStep(pixelsPerSecond);
    final end = math.max(lengthSeconds, size.width / pixelsPerSecond);
    final tick = Paint()..color = DawColors.textLow;
    for (var mark = 0.0; mark <= end + 0.001; mark += step) {
      final x = mark * pixelsPerSecond;
      canvas.drawLine(Offset(x, size.height - 8), Offset(x, size.height), tick);
      final label = TextPainter(
        text: TextSpan(text: formatClock(mark), style: const TextStyle(color: DawColors.textLow, fontSize: 10)),
        textDirection: TextDirection.ltr,
      )..layout();
      label.paint(canvas, Offset(x + 4, 6));
    }
    final playhead = positionSeconds * pixelsPerSecond;
    final head = Path()
      ..moveTo(playhead - 5, 0)
      ..lineTo(playhead + 5, 0)
      ..lineTo(playhead, 8)
      ..close();
    final accent = Paint()..color = DawColors.accent;
    canvas.drawPath(head, accent);
    canvas.drawLine(Offset(playhead, 0), Offset(playhead, size.height), accent..strokeWidth = 1.5);
    canvas.drawLine(Offset(0, size.height - 0.5), Offset(size.width, size.height - 0.5), Paint()..color = DawColors.border);
  }

  @override
  bool shouldRepaint(_RulerPainter oldDelegate) {
    return oldDelegate.positionSeconds != positionSeconds ||
        oldDelegate.pixelsPerSecond != pixelsPerSecond ||
        oldDelegate.lengthSeconds != lengthSeconds;
  }
}

class _LanePainter extends CustomPainter {
  _LanePainter({
    required this.channels,
    required this.clips,
    required this.lengthSeconds,
    required this.positionSeconds,
    required this.pixelsPerSecond,
  });

  final int channels;
  final List<ClipSpan> clips;
  final double lengthSeconds;
  final double positionSeconds;
  final double pixelsPerSecond;

  @override
  void paint(Canvas canvas, Size size) {
    final border = Paint()..color = DawColors.border;
    for (var channelIndex = 0; channelIndex < channels; channelIndex++) {
      final top = channelIndex * arrangementRowHeight;
      canvas.drawRect(
        Rect.fromLTWH(0, top, size.width, arrangementRowHeight),
        Paint()..color = channelIndex.isOdd ? DawColors.laneAlt : DawColors.appBg,
      );
      canvas.drawLine(Offset(0, top + arrangementRowHeight - 0.5), Offset(size.width, top + arrangementRowHeight - 0.5), border);
    }
    final step = rulerStep(pixelsPerSecond);
    final grid = Paint()..color = const Color(0x10FFFFFF);
    final end = math.max(lengthSeconds, size.width / pixelsPerSecond);
    for (var mark = step; mark <= end + 0.001; mark += step) {
      final x = mark * pixelsPerSecond;
      canvas.drawLine(Offset(x, 0), Offset(x, size.height), grid);
    }
    for (final clip in clips) {
      if (clip.channelIndex < 0 || clip.channelIndex >= channels) {
        continue;
      }
      final left = clip.start * pixelsPerSecond;
      final width = math.max(3.0, (clip.end - clip.start) * pixelsPerSecond);
      final rect = Rect.fromLTWH(left + 1, clip.channelIndex * arrangementRowHeight + 10, math.max(2, width - 2), arrangementRowHeight - 20);
      final color = _clipColors[clip.channelIndex % _clipColors.length];
      canvas.drawRRect(RRect.fromRectAndRadius(rect, const Radius.circular(4)), Paint()..color = color);
      if (rect.width > 36 && clip.name.isNotEmpty) {
        final label = TextPainter(
          text: TextSpan(
            text: clip.name,
            style: const TextStyle(color: DawColors.textHigh, fontSize: 12, fontWeight: FontWeight.w600),
          ),
          textDirection: TextDirection.ltr,
          maxLines: 1,
          ellipsis: '…',
        )..layout(maxWidth: rect.width - 12);
        label.paint(canvas, Offset(rect.left + 6, rect.top + (rect.height - label.height) / 2));
      }
    }
    final playhead = positionSeconds * pixelsPerSecond;
    canvas.drawLine(
      Offset(playhead, 0),
      Offset(playhead, size.height),
      Paint()
        ..color = DawColors.accent
        ..strokeWidth = 1.5,
    );
  }

  @override
  bool shouldRepaint(_LanePainter oldDelegate) {
    return oldDelegate.positionSeconds != positionSeconds ||
        oldDelegate.pixelsPerSecond != pixelsPerSecond ||
        oldDelegate.lengthSeconds != lengthSeconds ||
        oldDelegate.channels != channels ||
        oldDelegate.clips.length != clips.length;
  }
}
