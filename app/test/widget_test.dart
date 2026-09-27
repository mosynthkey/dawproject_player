import 'package:dawplay_app/main.dart';
import 'package:flutter_test/flutter_test.dart';

void main() {
  testWidgets('shows an empty project list', (tester) async {
    await tester.pumpWidget(const DawplayApp());
    await tester.pump();

    expect(find.text('Projects'), findsOneWidget);
    expect(find.text('No projects yet'), findsOneWidget);
    expect(find.text('Add a .dawproject on the left.'), findsOneWidget);
  });
}
