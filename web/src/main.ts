import { createApp } from 'vue'
import App from './App.vue'
import './style.css'
import { connect } from './composables/useEms'

connect()
createApp(App).mount('#app')
