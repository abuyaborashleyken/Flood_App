const installButton = document.createElement('button');
const headerTools = document.querySelector('.header-tools');

if (headerTools && 'serviceWorker' in navigator && window.isSecureContext) {
  let deferredInstallPrompt = null;
  const isInstalled = window.matchMedia('(display-mode: standalone)').matches || navigator.standalone === true;
  const installDialog = document.createElement('dialog');
  installDialog.id = 'installInstructionsDialog';
  installDialog.className = 'history-dialog install-dialog';
  installDialog.setAttribute('aria-labelledby', 'installDialogTitle');
  installDialog.setAttribute('aria-describedby', 'installDialogDescription');
  installDialog.innerHTML = `
    <div class="history-head">
      <div>
        <h2 id="installDialogTitle">Install S.E.C.U.R.E-App</h2>
        <p id="installDialogDescription">Add the water monitor to your home screen for quick access.</p>
      </div>
      <button class="history-close" id="installDialogClose" type="button" aria-label="Close install instructions">&times;</button>
    </div>
    <ol class="install-instructions" id="installInstructions"></ol>
  `;
  document.body.append(installDialog);

  installButton.className = 'install-app-button';
  installButton.id = 'installAppButton';
  installButton.type = 'button';
  installButton.textContent = 'Install app';
  installButton.setAttribute('aria-haspopup', 'dialog');
  installButton.setAttribute('aria-controls', installDialog.id);
  installButton.hidden = isInstalled;
  headerTools.insertBefore(installButton, headerTools.querySelector('.connection-clock'));

  function showInstallInstructions() {
    const instructions = document.getElementById('installInstructions');
    const userAgent = navigator.userAgent;
    const isAppleMobile = /iPhone|iPad|iPod/i.test(userAgent)
      || (navigator.platform === 'MacIntel' && navigator.maxTouchPoints > 1);
    const steps = isAppleMobile
      ? ['Open this page in Safari.', 'Tap Share, then choose Add to Home Screen.', 'Tap Add to finish installing.']
      : /Android/i.test(userAgent)
        ? ['Open this page in your browser.', 'Open the browser menu and choose Install app or Add to Home screen.', 'Confirm the installation.']
        : ['Open this page in Chrome, Edge, or another supported browser.', 'Use the install icon in the address bar, or choose Install app from the browser menu.'];

    instructions.replaceChildren(...steps.map(step => {
      const item = document.createElement('li');
      item.textContent = step;
      return item;
    }));

    if (typeof installDialog.showModal === 'function') {
      installDialog.showModal();
    } else {
      window.alert(steps.join('\n'));
    }
  }

  installButton.addEventListener('click', async () => {
    if (!deferredInstallPrompt) {
      showInstallInstructions();
      return;
    }

    const prompt = deferredInstallPrompt;
    deferredInstallPrompt = null;
    try {
      await prompt.prompt();
      const choice = await prompt.userChoice;
      if (choice.outcome === 'accepted') {
        installButton.hidden = true;
      } else {
        showInstallInstructions();
      }
    } catch (error) {
      console.error('Could not open the app installation prompt.', error);
      showInstallInstructions();
    }
  });

  document.getElementById('installDialogClose').addEventListener('click', () => installDialog.close());
  installDialog.addEventListener('click', event => {
    if (event.target === installDialog) installDialog.close();
  });

  window.addEventListener('beforeinstallprompt', event => {
    event.preventDefault();
    deferredInstallPrompt = event;
    installButton.hidden = false;
  });

  window.addEventListener('appinstalled', () => {
    deferredInstallPrompt = null;
    installButton.hidden = true;
    if (installDialog.open) installDialog.close();
  });

  navigator.serviceWorker.register('/service-worker.js').catch(error => {
    console.error('Could not register the S.E.C.U.R.E-App offline service worker.', error);
  });
}
