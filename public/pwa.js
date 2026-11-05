const headerTools = document.querySelector('.header-tools');

if (headerTools && 'serviceWorker' in navigator && window.isSecureContext) {
  let deferredInstallPrompt = null;
  const isInstalled = window.matchMedia('(display-mode: standalone)').matches || navigator.standalone === true;
  const installDialog = document.createElement('dialog');
  installDialog.id = 'installInstructionsDialog';
  installDialog.className = 'install-dialog';
  installDialog.setAttribute('aria-labelledby', 'installDialogTitle');
  installDialog.setAttribute('aria-describedby', 'installDialogDescription');
  installDialog.innerHTML = `
    <div class="install-dialog-header">
      <div class="install-dialog-icon" aria-hidden="true">
        <svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="1.8" stroke-linecap="round" stroke-linejoin="round">
          <rect x="5" y="2.75" width="14" height="18.5" rx="2.5"></rect>
          <path d="M10 18h4"></path>
        </svg>
      </div>
      <div class="install-dialog-heading">
        <p class="install-dialog-kicker">Quick setup guide</p>
        <h2 id="installDialogTitle">Install S.E.C.U.R.E APPLICATION</h2>
        <p class="install-dialog-description" id="installDialogDescription">Add the water monitor to your device for easy access. Choose the instructions for your device below.</p>
      </div>
      <button class="install-dialog-close" id="installDialogClose" type="button" aria-label="Close installation guide">&times;</button>
    </div>
    <div class="install-platforms" aria-label="Installation instructions by device">
      <section class="install-platform" data-platform="android" aria-labelledby="installAndroidTitle">
        <div class="install-platform-top">
          <h3 id="installAndroidTitle">Android</h3>
          <span class="install-platform-badge" hidden></span>
        </div>
        <ol>
          <li>Open this page in Chrome or Samsung Internet.</li>
          <li>In Chrome, tap <strong>Install app</strong> or <strong>Add to Home screen</strong> in the menu. In Samsung Internet, tap <strong>Menu</strong>, then <strong>Add page to</strong> and <strong>Home screen</strong>.</li>
          <li>Confirm <strong>Install</strong> or <strong>Add</strong>. Open S.E.C.U.R.E from your home screen or app list.</li>
        </ol>
      </section>
      <section class="install-platform" data-platform="ios" aria-labelledby="installIosTitle">
        <div class="install-platform-top">
          <h3 id="installIosTitle">iPhone &amp; iPad</h3>
          <span class="install-platform-badge" hidden></span>
        </div>
        <ol>
          <li>Open this page in <strong>Safari</strong>. If you are in another browser, open the page in Safari first.</li>
          <li>Tap <strong>Share</strong> <span aria-label="the square with an upward arrow">(&#x2191;)</span>, then scroll and tap <strong>Add to Home Screen</strong>.</li>
          <li>Tap <strong>Add</strong>. S.E.C.U.R.E will appear on your home screen.</li>
        </ol>
      </section>
      <section class="install-platform" data-platform="desktop" aria-labelledby="installDesktopTitle">
        <div class="install-platform-top">
          <h3 id="installDesktopTitle">Windows, Mac &amp; Linux</h3>
          <span class="install-platform-badge" hidden></span>
        </div>
        <ol>
          <li>Open this page in a supported browser such as Chrome or Microsoft Edge.</li>
          <li>Select the <strong>Install</strong> icon in the address bar. Or, open the browser menu and choose <strong>Install S.E.C.U.R.E APPLICATION</strong> (in Edge, look under <strong>Apps</strong>).</li>
          <li>Confirm <strong>Install</strong>. Find S.E.C.U.R.E in your apps list or Start menu.</li>
        </ol>
      </section>
    </div>
    <div class="install-dialog-footer">
      <button class="install-native-button" id="installNativeButton" type="button" hidden>Install on this device</button>
      <p class="install-help">You need an internet connection to view live readings. Installation is available in supported browsers when this page is open over HTTPS.</p>
      <p class="install-status" id="installStatus" role="status" aria-live="polite"></p>
    </div>
  `;
  document.body.append(installDialog);

  const installButton = document.createElement('button');
  installButton.className = 'install-app-button';
  installButton.id = 'installAppButton';
  installButton.type = 'button';
  installButton.innerHTML = '<svg class="header-btn-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.2" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4"></path><polyline points="7 10 12 15 17 10"></polyline><line x1="12" y1="15" x2="12" y2="3"></line></svg><span>Install app</span>';
  installButton.setAttribute('aria-haspopup', 'dialog');
  installButton.setAttribute('aria-controls', installDialog.id);
  installButton.hidden = isInstalled;
  headerTools.insertBefore(installButton, headerTools.querySelector('.connection-clock'));

  const nativeInstallButton = document.getElementById('installNativeButton');
  const installStatus = document.getElementById('installStatus');

  function highlightCurrentDevice() {
    const userAgent = navigator.userAgent;
    const platform = /Android/i.test(userAgent)
      ? 'android'
      : /iPhone|iPad|iPod/i.test(userAgent)
        || (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1)
        ? 'ios'
        : 'desktop';
    const currentPlatform = installDialog.querySelector(`[data-platform="${platform}"]`);
    if (!currentPlatform) return;

    currentPlatform.classList.add('install-platform-current');
    const badge = currentPlatform.querySelector('.install-platform-badge');
    badge.textContent = 'This device';
    badge.hidden = false;
  }

  function showInstallGuide() {
    highlightCurrentDevice();
    installStatus.textContent = '';
    if (typeof installDialog.showModal === 'function') {
      installDialog.showModal();
      return;
    }

    installDialog.setAttribute('open', '');
    document.getElementById('installDialogClose').focus();
  }

  installButton.addEventListener('click', showInstallGuide);

  nativeInstallButton.addEventListener('click', async () => {
    if (!deferredInstallPrompt) {
      installStatus.textContent = 'Use the steps above to install this app in your browser.';
      return;
    }

    const prompt = deferredInstallPrompt;
    deferredInstallPrompt = null;
    nativeInstallButton.hidden = true;
    try {
      await prompt.prompt();
      const choice = await prompt.userChoice;
      if (choice.outcome === 'accepted') {
        installButton.hidden = true;
        installStatus.textContent = 'S.E.C.U.R.E APPLICATION is being installed on this device.';
      } else {
        installStatus.textContent = 'Installation was not started. You can try again or follow the steps above.';
      }
    } catch (error) {
      console.error('Could not open the app installation prompt.', error);
      installStatus.textContent = 'The installation prompt could not be opened. Follow the steps above to install from your browser.';
    }
  });

  document.getElementById('installDialogClose').addEventListener('click', () => {
    if (typeof installDialog.close === 'function') installDialog.close();
    else installDialog.removeAttribute('open');
  });
  installDialog.addEventListener('click', event => {
    if (event.target === installDialog && typeof installDialog.close === 'function') installDialog.close();
  });
  installDialog.addEventListener('close', () => {
    if (!installButton.hidden) installButton.focus();
  });

  window.addEventListener('beforeinstallprompt', event => {
    event.preventDefault();
    deferredInstallPrompt = event;
    nativeInstallButton.hidden = false;
  });

  window.addEventListener('appinstalled', () => {
    deferredInstallPrompt = null;
    nativeInstallButton.hidden = true;
    installButton.hidden = true;
    if (installDialog.open && typeof installDialog.close === 'function') installDialog.close();
  });

  navigator.serviceWorker.register('/service-worker.js').catch(error => {
    console.error('Could not register the S.E.C.U.R.E-App offline service worker.', error);
  });
}
