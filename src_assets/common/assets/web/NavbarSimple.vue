<template>
  <nav class="navbar navbar-sunshine">
    <div class="container-fluid">
      <!-- MEOW-TOUCH(web-identity): brand tooltip and image alt, as in Navbar.vue. -->
      <span class="navbar-brand" title="Sunmeow">
        <img src="/images/logo-sunshine-45.png" height="45" alt="Sunmeow">
      </span>
      <ThemeToggle/>
    </div>
  </nav>
</template>

<script>
import ThemeToggle from './ThemeToggle.vue'

export default {
  components: {
    ThemeToggle,
  },
}
</script>
