package com.logx.tester

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.viewModels
import com.logx.tester.ui.TesterScreen
import com.logx.tester.ui.theme.LogXTesterTheme

class MainActivity : ComponentActivity() {
    private val viewModel: TesterViewModel by viewModels()

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContent {
            LogXTesterTheme {
                TesterScreen(viewModel = viewModel)
            }
        }
    }
}

