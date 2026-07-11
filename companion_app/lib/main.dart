import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import 'theme.dart';
import 'screens/dashboard_screen.dart';
import 'state/draupnir_state.dart';

void main() {
  runApp(
    MultiProvider(
      providers: [
        ChangeNotifierProvider(create: (_) => DraupnirState()),
      ],
      child: const DraupnirApp(),
    ),
  );
}

class DraupnirApp extends StatelessWidget {
  const DraupnirApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Draupnir Companion',
      debugShowCheckedModeBanner: false,
      theme: AppTheme.darkTheme,
      home: const DashboardScreen(),
    );
  }
}
