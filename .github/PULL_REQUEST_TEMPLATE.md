<!--
Plumb takes spam-policy filters, and fixes to the filters it ships. Changes
to anything else belong upstream in Bitcoin Knots or Bitcoin Core.

The bar is in the README under "Submitting a filter". Please cover:

- The embedding shape it catches, with a transaction id or two.
- The option name. It should default on and be turned off by -corepolicy.
- The tests that cover it.
- Chain numbers: block range scanned, how many transactions it rejects, and
  what you found when you looked for payments among them.
- The Knots pull request, if there is one.
-->
