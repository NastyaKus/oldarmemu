package org.oldarmemu.hd2.ui

import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Home
import androidx.compose.material.icons.filled.Settings
import androidx.compose.material3.Icon
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.navigation.NavGraph.Companion.findStartDestination
import androidx.navigation.compose.NavHost
import androidx.navigation.compose.composable
import androidx.navigation.compose.currentBackStackEntryAsState
import androidx.navigation.compose.rememberNavController

class Actions(
    val importRecovery: () -> Unit,
    val addToSd: () -> Unit,
    val backupNand: () -> Unit,
    val restoreNand: () -> Unit,
    val shareLogs: () -> Unit,
)

private data class Tab(val route: String, val label: String, val icon: ImageVector)

private val tabs = listOf(
    Tab("home", "Телефон", Icons.Filled.Home),
    Tab("sd", "SD-карта", Hd2Icons.SdCard),
    Tab("settings", "Настройки", Icons.Filled.Settings),
)

@Composable
fun App(state: AppState, act: Actions) {
    val nav = rememberNavController()
    val entry by nav.currentBackStackEntryAsState()
    val route = entry?.destination?.route
    Scaffold(
        bottomBar = {
            NavigationBar {
                for (t in tabs) {
                    NavigationBarItem(
                        selected = route == t.route,
                        onClick = {
                            nav.navigate(t.route) {
                                popUpTo(nav.graph.findStartDestination().id) { saveState = true }
                                launchSingleTop = true
                                restoreState = true
                            }
                        },
                        icon = { Icon(t.icon, null) },
                        label = { Text(t.label) },
                    )
                }
            }
        },
    ) { pad ->
        Box(Modifier.fillMaxSize().padding(pad)
            .windowInsetsPadding(WindowInsets.safeDrawing.only(WindowInsetsSides.Top))) {
            NavHost(nav, startDestination = "home") {
                composable("home") { HomeScreen(state, act.importRecovery, act.addToSd) }
                composable("sd") { SdScreen(state, act.addToSd) }
                composable("settings") {
                    SettingsScreen(state, act.backupNand, act.restoreNand, act.shareLogs) {
                        nav.navigate("log/$it")
                    }
                }
                composable("log/{which}") { e ->
                    LogScreen(state, e.arguments?.getString("which") ?: "console", act.shareLogs)
                }
            }
        }
    }
    BusyDialog(state)
    MessageDialog(state)
}
